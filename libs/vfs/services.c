/*===========================================================================
 *
 *                            PUBLIC DOMAIN NOTICE
 *               National Center for Biotechnology Information
 *
 *  This software/database is a "United States Government Work" under the
 *  terms of the United States Copyright Act.  It was written as part of
 *  the author's official duties as a United States Government employee and
 *  thus cannot be copyrighted.  This software/database is freely available
 *  to the public for use. The National Library of Medicine and the U.S.
 *  Government have not placed any restriction on its use or reproduction.
 *
 *  Although all reasonable efforts have been taken to ensure the accuracy
 *  and reliability of the software and data, the NLM and the U.S.
 *  Government do not and cannot warrant the performance or results that
 *  may be obtained by using this software or data. The NLM and the U.S.
 *  Government disclaim all warranties, express or implied, including
 *  warranties of performance, merchantability or fitness for any particular
 *  purpose.
 *
 *  Please cite the author in any work or product based on this material.
 *
 * ===========================================================================
 *
 */

#include <vfs/extern.h>
#include <kfg/kart-priv.h> /* KartItemMake2 */
#include <klib/debug.h> /* DBGMSG */
#include <klib/log.h> /* KLogLevel */
#include <klib/out.h> /* KOutMsg */
#include <klib/printf.h> /* string_printf */
#include <klib/rc.h> /* RC */
#include <klib/text.h> /* String */
#include <klib/vector.h> /* Vector */
#include <kfg/config.h> /* KConfigRelease */
#include <kfg/repository.h> /* KRepositoryMgrRelease */
#include <kns/http.h> /* KHttpRequest */
#include <kns/kns-mgr-priv.h> /* KNSManagerMakeReliableClientRequest */
#include <kns/manager.h> /* KNSManager */
#include <kns/stream.h> /* KBufferStreamMake */
#include <kproc/timeout.h> /* TimeoutInit */
#include <vfs/manager.h> /* VFSManager */
#include <vfs/pathsetlist.h> /* KSrvResponseGetPath */
#include <vfs/services.h> /* KServiceMake */

#include "path-priv.h" /* VPathMakeFmt */
#include "services-priv.h"
#include "resolver-priv.h" /* VPathCheckFromNamesCGI */

#include <ctype.h> /* isdigit */
#include <string.h> /* memset */

#define RELEASE(type, obj) do { rc_t rc2 = type##Release(obj); \
    if (rc2 && !rc) { rc = rc2; } obj = NULL; } while (false)


typedef enum {
    eOT_undefined,
    eOT_empty,
    eOT_dbgap,
    eOT_provisional,
    eOT_srapub,
    eOT_sragap,
    eOT_srapub_source,
    eOT_sragap_source,
    eOT_srapub_files,
    eOT_sragap_files,
    eOT_refseq,
    eOT_wgs,
    eOT_na,
    eOT_nakmer,
} EObjectType;

typedef enum {
    eSTnames,
    eSTsearch,
} EServiceType;

/* request/response/processing helper objects */
typedef struct {
    struct       KConfig        * kfg;
    const struct KNSManager     * kMgr;
    const struct KRepositoryMgr * repoMgr;
    uint32_t timeoutMs;
} SHelper;

/* raw string text */
typedef struct { char * s; } SRaw; 

/* version from server response */
#define VERSION_1_0 0x01000000
#define VERSION_1_1 0x01010000
#define VERSION_1_2 0x01020000
#define VERSION_3_0 0x03000000
#define VERSION_3_1 0x03010000
#define VERSION_3_2 0x03020000

typedef struct {
    SRaw raw;
    ver_t version;
    uint8_t major;
    uint8_t minor;
} SVersion; 

/* server response header */
typedef struct {
    SRaw raw;
    SVersion version;
} SHeader;

#define N_NAMES1_0 5
#define N_NAMES1_1 10
#define N_NAMES3_0  9
#define N_NAMES3_1 17
#define N_NAMES3_2 12

/* response row parsed into array of Strings */
typedef struct {
    int n;
    String s [ N_NAMES3_1 ];
} SOrdered;

/* response row parsed into named typed fields */
typedef struct {
    bool inited;
    uint32_t ordId;
    EObjectType objectType;
    String accession; /* versios 1.1/1.2 only */
    String objectId;
    String name;
    size_t size;
    KTime_t date;
    uint8_t md5 [ 16 ];
    String ticket;
    String url;
    String hUrl;
    String fpUrl;
    String hsUrl;
    String flUrl;
    String s3Url;
    String vdbcacheUrl;
    String hVdbcacheUrl;
    String fpVdbcacheUrl;
    String hsVdbcacheUrl;
    String flVdbcacheUrl;
    String s3VdbcacheUrl;
    KTime_t expiration;
    uint32_t code;
    String message;
} STyped;

/* converter from server response string to an object */
typedef rc_t TConverter ( void * dest, const String * src );
typedef struct { TConverter * f; } SConverter;
typedef void * TFieldGetter ( STyped * self, int n );

typedef struct {
    int n;
    TFieldGetter * get;
    TConverter ** f;
} SConverters;

/* a response row */
typedef struct {
    SRaw raw;
    SOrdered ordered;
    STyped typed;
    EVPath path;
    VPathSet * set;
} SRow;

/* server response = header + vector of response rows */
typedef struct {
    EServiceType serviceType;
    SHeader header;
    Vector rows;
    KSrvResponse * list;
    Kart * kart;
} SResponse;

/* request info */

/* key-value */
typedef struct {
    String k;
    String v;
} SKV;

typedef struct {
    KHttpRequest * req;
    rc_t rc;
} SHttpRequestHelper;

typedef struct {
    bool inited;
    char * cgi;
    Vector params;
} SCgiRequest;

typedef struct {
    char * objectId;
    EObjectType objectType;
} SObject;

typedef struct {
    Vector tickets;
    KDataBuffer str;
    size_t size;
    rc_t rc;
} STickets;

typedef struct {
    SObject object [ 256 ];
    uint32_t objects;
    bool refseq_ctx;
} SRequestData;

typedef struct {
    EServiceType serviceType;
    SVersion version;
    SCgiRequest cgiReq;
    SRequestData request;
    STickets tickets;
    int errorsToIgnore;
    VRemoteProtocols protocols;
} SRequest;

/* service object */
struct KService {
    SHelper helper;
    SRequest req;
    SResponse resp;
};


/* SHelper */
static rc_t SHelperInit ( SHelper * self, const KNSManager * mgr ) {
    rc_t rc = 0;
    assert ( self );
    memset ( self, 0, sizeof * self );
    if ( mgr == NULL ) {
        KNSManager * kns = NULL;
        rc = KNSManagerMake ( & kns );
        mgr = kns;
    }
    else {
        rc = KNSManagerAddRef ( mgr );
    }
    if ( rc == 0) {
        self -> kMgr = mgr;
    }
    self -> timeoutMs = 5000;
    return rc;
}

static rc_t SHelperFini ( SHelper * self) {
    rc_t rc = 0;

    assert ( self );

    RELEASE ( KConfig       , self -> kfg );
    RELEASE ( KNSManager    , self -> kMgr );
    RELEASE ( KRepositoryMgr, self -> repoMgr );

    memset ( self, 0, sizeof * self );

    return rc;
}

VRemoteProtocols SHelperDefaultProtocols ( SHelper * self ) {
    assert ( self );

    VRemoteProtocols protocols = DEFAULT_PROTOCOLS;

    if ( self -> kfg == NULL ) {
        KConfigMake ( & self -> kfg, NULL );
    }

    KConfigReadRemoteProtocols ( self -> kfg, & protocols );

    return protocols;
}

rc_t SHelperProjectToTicket ( SHelper * self, uint32_t projectId,
    char * buffer, size_t bsize, size_t * ticket_size )
{
    rc_t rc = 0;

    const KRepository * repo = NULL;

    assert ( self );

    if ( self -> repoMgr == NULL ) {
        if ( self -> kfg == NULL ) {
            rc = KConfigMake ( & self -> kfg, NULL );
            if ( rc != 0 )
                return rc;
        }

        rc = KConfigMakeRepositoryMgrRead ( self -> kfg, & self -> repoMgr );
        if ( rc != 0 )
            return rc;
    }

    rc = KRepositoryMgrGetProtectedRepository ( self -> repoMgr, projectId,
        & repo );
    if ( rc != 0 )
        return rc;

    rc = KRepositoryDownloadTicket ( repo, buffer, bsize, ticket_size );

    RELEASE ( KRepository, repo );

    return rc;
}


/* SRaw */
static void SRawInit ( SRaw * self, char * s ) {
    assert ( self );
    self -> s = s;
}

static rc_t SRawAlloc ( SRaw * self, const char * s ) {
    char * p = string_dup_measure ( s, NULL );
    if ( p == NULL ) {
        return RC ( rcVFS, rcPath, rcAllocating, rcMemory, rcExhausted );
    }
    SRawInit ( self, p );
    return 0;
}

static rc_t SRawFini ( SRaw * self ) {
    if ( self != NULL ) {
        free ( self -> s );
        self -> s = NULL;
    }
    return 0;
}


/* SVersion */
static rc_t SVersionInit
    ( SVersion * self, const char * src, EServiceType serviceType )
{
    const char * s = src;

    assert ( self );

    memset ( self, 0, sizeof * self );

    if ( s == NULL ) {
        return RC ( rcVFS, rcQuery, rcExecuting, rcMessage, rcBadVersion );
    }

    if ( * s != '#' ) {
        if ( serviceType != eSTnames || * s == '\0' || ! isdigit ( * s ) )
            return RC ( rcVFS, rcQuery, rcExecuting, rcMessage, rcBadVersion );
    }
    else
        ++ s;

    if ( serviceType == eSTsearch ) {
        const char version [] = "version ";
        size_t sz = sizeof version - 1;
        if ( string_cmp ( s, sz, version, sz, ( uint32_t ) sz ) != 0 )
            return RC ( rcVFS, rcQuery, rcExecuting, rcMessage, rcBadVersion );
        s += sz;
    }

    if ( * s == '\0' ) {
        return RC ( rcVFS, rcQuery, rcExecuting, rcMessage, rcBadVersion );
    }

    {
        char * end = NULL;
        uint64_t l = strtoul ( s, & end, 10 );
        if ( end == NULL || * end != '.' ) {
            return RC ( rcVFS, rcQuery, rcResolving, rcMessage, rcCorrupt );
        }
        self -> major = l;
        s = ++ end;

        l = strtoul ( s, & end, 10 );
        if ( end == NULL || * end != '\0' ) {
            return RC ( rcVFS, rcQuery, rcResolving, rcMessage, rcCorrupt );
        }
        self -> minor = l;

        self -> version = self -> major << 24 | self -> minor << 16;

        return SRawAlloc ( & self -> raw, src );
    }
}

static rc_t SVersionFini ( SVersion * self ) {
    rc_t rc = 0;
    assert ( self );
    rc = SRawFini ( & self -> raw );
    memset ( self, 0, sizeof * self );
    return rc;
}

static rc_t SVersionToString ( const SVersion * self, char ** s ) {
    size_t num_writ = 0;
    char tmp [ 1 ];
    assert ( self && s );
    string_printf ( tmp, 1, & num_writ, "%u.%u", self -> major, self -> minor );
    ++ num_writ;
    * s = ( char * ) malloc ( num_writ );
    if ( * s == NULL ) {
        return RC ( rcVFS, rcQuery, rcExecuting, rcMemory, rcExhausted );
    }
    return string_printf ( * s, num_writ,
        & num_writ, "%u.%u", self -> major, self -> minor );
}


/* SHeader */
static rc_t SHeaderMake
    ( SHeader * self, char * src, EServiceType serviceType )
{
    assert ( self );
    memset ( self, 0, sizeof * self );
    SRawInit ( & self -> raw, src );
    return SVersionInit ( & self -> version, self -> raw . s, serviceType );
}

static rc_t SHeaderFini ( SHeader * self ) {
    rc_t rc = 0;
    if ( self != NULL ) {
        rc_t r2 = SRawFini ( & self -> raw );
        rc = SVersionFini ( & self -> version );
        if ( rc == 0 ) {
            rc = r2;
        }
    }
    return rc;
}


/* CONVERTERS */
/* functions to initialize objects from response row field Strings */
static
bool cmpStringAndObjectType ( const String * s, const char * val )
{
    size_t sz = string_size (val );
    String v;
    StringInit ( & v, val, sz, sz );
    return StringCompare ( s, & v ) == 0;
}

/* N.B. DO NOT FREE RETURNED STRING !!! */
static const char * ObjectTypeToString ( EObjectType self ) {
    switch ( self ) {
        case eOT_undefined   : return "";
        case eOT_dbgap       : return "dbgap";
        case eOT_provisional : return "provisional";
        case eOT_srapub      : return "srapub";
        case eOT_sragap      : return "sragap";
        case eOT_srapub_source:return "srapub_source";
        case eOT_sragap_source:return "sragap_source";
        case eOT_srapub_files: return "srapub_files";
        case eOT_sragap_files: return "sragap_files";
        case eOT_refseq      : return "refseq";
        case eOT_wgs         : return "wgs";
        case eOT_na          : return "na";
        case eOT_nakmer      : return "nakmer";
        default: assert ( 0 ); return "";
    }
}

static EObjectType StringToObjectType ( const String * s ) {
    if ( cmpStringAndObjectType ( s, "" ) ) {
        return eOT_empty;
    }
    if ( cmpStringAndObjectType ( s, "dbgap" ) ) {
        return eOT_dbgap;
    }
    if ( cmpStringAndObjectType ( s, "provisional" ) ) {
        return eOT_provisional;
    }
    if ( cmpStringAndObjectType ( s, "srapub" ) ) {
        return eOT_srapub;
    }
    if ( cmpStringAndObjectType ( s, "sragap" ) ) {
        return eOT_sragap;
    }
    if ( cmpStringAndObjectType ( s, "srapub_source" ) ) {
        return eOT_srapub_source;
    }
    if ( cmpStringAndObjectType ( s, "srapub_files" ) ) {
        return eOT_srapub_files;
    }
    if ( cmpStringAndObjectType ( s, "sragap_files") ) {
        return eOT_sragap_files;
    }
    if ( cmpStringAndObjectType ( s, "refseq") ) {
        return eOT_refseq;
    }
    if ( cmpStringAndObjectType ( s, "wgs") ) {
        return eOT_wgs;
    }
    if ( cmpStringAndObjectType ( s, "na") ) {
        return eOT_na;
    }
    if ( cmpStringAndObjectType ( s, "nakmer") ) {
        return eOT_nakmer;
    }
    return eOT_undefined;
}

static rc_t EObjectTypeInit ( void * p, const String * src ) {
    EObjectType * self = ( EObjectType * ) p;
    EObjectType t = StringToObjectType ( src );
    if ( t == eOT_undefined ) {
        return RC ( rcVFS, rcQuery, rcExecuting, rcType, rcIncorrect );
    }
    assert ( self );
    * self = t;
    return 0;
}

static rc_t aStringInit ( void * p, const String * src ) {
    String * self = ( String * ) p;
    assert ( src );
    StringInit ( self, src -> addr, src -> size, src -> len );
    return 0;
}

static rc_t size_tInit ( void * p, const String * src ) {
    rc_t rc = 0;
    size_t * self = ( size_t * ) p;
    size_t s = 0;
    if ( src -> size != 0  && src -> len != 0 ) {
        s = StringToU64 ( src, & rc );
    }
    if ( rc == 0 ) {
        assert ( self );
        * self = s;
    }
    return rc;
}

static rc_t uint32_tInit ( void * p, const String * src ) {
    rc_t rc = 0;
    uint32_t * self = ( uint32_t * ) p;
    uint32_t s = StringToU64 ( src, & rc );
    if ( rc == 0 ) {
        assert ( self );
        * self = s;
    }
    return rc;
}

static rc_t KTimeInit ( void * p, const String * src ) { return 0; } /* TODO */
static rc_t md5Init ( void * p, const String * src ) { return 0; }   /* TODO */

/* converter from names-1.0 response row to STyped object  */
static void * STypedGetFieldNames1_0 ( STyped * self, int n ) {
    assert ( self );
    switch ( n ) {
        case  0: return & self -> accession;
        case  1: return & self -> ticket;
        case  2: return & self -> hUrl;
        case  3: return & self -> code;
        case  4: return & self -> message;
    }
    return 0;
}

static const SConverters * SConvertersNames1_0Make ( void ) {
    static TConverter * f [ N_NAMES1_0 + 1 ] = {
        aStringInit,
        aStringInit,
        aStringInit,
        uint32_tInit,
        aStringInit, NULL };
    static const SConverters c = {
        N_NAMES1_0,
        STypedGetFieldNames1_0, f };
    return & c;
}

/* converter from names-1.1 response row to STyped object  */
static void * STypedGetFieldNames1_1 ( STyped * self, int n ) {
    assert ( self);
    switch ( n ) {
        case  0: return & self -> accession;
        case  1: return & self -> objectId;
        case  2: return & self -> name;
        case  3: return & self -> size;
        case  4: return & self -> date;
        case  5: return & self -> md5;
        case  6: return & self -> ticket;
        case  7: return & self -> hUrl;
        case  8: return & self -> code;
        case  9: return & self -> message;
    }
    return 0;
}

static const SConverters * SConvertersNames1_1Make ( void ) {
    static TConverter * f [ N_NAMES1_1 + 1 ] = {
        aStringInit,
        aStringInit,
        aStringInit,
        size_tInit,
        KTimeInit,
        md5Init,
        aStringInit,
        aStringInit,
        uint32_tInit,
        aStringInit, NULL };
    static const SConverters c = {
        N_NAMES1_1,
        STypedGetFieldNames1_1, f };
    return & c;
}

static const SConverters * SConvertersNames1_2Make ( void ) {
    static TConverter * f [ N_NAMES1_1 + 1 ] = {
        aStringInit,
        aStringInit,
        aStringInit,
        size_tInit,
        KTimeInit,
        md5Init,
        aStringInit,
        aStringInit,
        uint32_tInit,
        aStringInit, NULL };
    static const SConverters c = {
        N_NAMES1_1,
        STypedGetFieldNames1_1, f };
    return & c;
}

/* converter from current names-3.0 response row to STyped object  */
static void * STypedGetFieldNames3_0 ( STyped * self, int n ) {
    assert ( self);
    switch ( n ) {
        case  0: return & self -> objectType;
        case  1: return & self -> objectId;
        case  2: return & self -> size;
        case  3: return & self -> date;
        case  4: return & self -> md5;
        case  5: return & self -> ticket;
        case  6: return & self -> hUrl;
        case  7: return & self -> code;
        case  8: return & self -> message;
    }
    return 0;
}

static const SConverters * SConvertersNames3_0Make ( void ) {
    static TConverter * f [ N_NAMES3_0 + 1 ] = {
        EObjectTypeInit,
#if 0
        aStringInit,
#endif
        aStringInit,
        size_tInit,
        KTimeInit,
        md5Init,
        aStringInit,
        aStringInit,
        uint32_tInit,
        aStringInit, NULL };
    static const SConverters c = {
        N_NAMES3_0,
        STypedGetFieldNames3_0, f };
    return & c;
}

/* converter from proposed names-3.0 response row to STyped object  */
static void * STypedGetFieldNames3_1 ( STyped * self, int n ) {
    assert ( self);
    switch ( n ) {
        case  0: return & self -> objectType;
        case  1: return & self -> objectId;
        case  2: return & self -> size;
        case  3: return & self -> date;
        case  4: return & self -> md5;
        case  5: return & self -> ticket;
        case  6: return & self -> hUrl;
        case  7: return & self -> fpUrl;
        case  8: return & self -> hsUrl;
        case  9: return & self -> flUrl;
        case 10: return & self -> hVdbcacheUrl;
        case 11: return & self -> fpVdbcacheUrl;
        case 12: return & self -> hsVdbcacheUrl;
        case 13: return & self -> flVdbcacheUrl;
        case 14: return & self -> expiration;
        case 15: return & self -> code;
        case 16: return & self -> message;
    }
    return 0;
}

static const SConverters * SConvertersNames3_1Make ( void ) {
    static TConverter * f [ N_NAMES3_1 + 1 ] = {
        EObjectTypeInit,
        aStringInit,
        size_tInit,
        KTimeInit,
        md5Init,
        aStringInit,
        aStringInit,
        aStringInit,
        aStringInit,
        aStringInit,
        aStringInit,
        aStringInit,
        aStringInit,
        aStringInit,
        KTimeInit,
        uint32_tInit,
        aStringInit, NULL };
    static const SConverters c = {
        N_NAMES3_1,
        STypedGetFieldNames3_1, f };
    return & c;
}

/* converter from proposed names-3.0 response row to STyped object  */
static void * STypedGetFieldNames3_2 ( STyped * self, int n ) {
    assert ( self);
    switch ( n ) {
        case  0: return & self -> ordId;
        case  1: return & self -> objectType;
        case  2: return & self -> objectId;
        case  3: return & self -> size;
        case  4: return & self -> date;
        case  5: return & self -> md5;
        case  6: return & self -> ticket;
        case  7: return & self -> url;
        case  8: return & self -> vdbcacheUrl;
        case  9: return & self -> expiration;
        case 10: return & self -> code;
        case 11: return & self -> message;
    }
    return 0;
}

static const SConverters * SConvertersNames3_2Make ( void ) {
    static TConverter * f [ N_NAMES3_2 + 1 ] = {
        uint32_tInit,    /*  0 ord-id */
        EObjectTypeInit, /*  1 object-type */
        aStringInit,     /*  2 object-id */
        size_tInit,      /*  3 size */
        KTimeInit,       /*  4 date */
        md5Init,         /*  5 md5 */
        aStringInit,     /*  6 ticket */
        aStringInit,     /*  7 url */
        aStringInit,     /*  8 vdbcache-url */
        KTimeInit,       /* 9 expiration */
        uint32_tInit,    /* 10 status-code */
        aStringInit,     /* 11 message */
        NULL };
    static const SConverters c = {
        N_NAMES3_2,
        STypedGetFieldNames3_2, f };
    return & c;
}

/* converter factory function */
static
rc_t SConvertersMake ( const SConverters ** self, SHeader * header )
{
    assert ( self && header );
    switch ( header -> version. version ) {
        case VERSION_1_0:
            * self = SConvertersNames1_0Make ();
            return 0;
        case VERSION_1_1:
            * self = SConvertersNames1_1Make ();
            return 0;
        case VERSION_1_2:
            * self = SConvertersNames1_2Make ();
            return 0;
        case VERSION_3_0:
            * self = SConvertersNames3_0Make ();
            return 0;
        case VERSION_3_1:
            * self = SConvertersNames3_1Make ();
            return 0;
        case VERSION_3_2:
            * self = SConvertersNames3_2Make ();
            return 0;
        default:
            * self = NULL;
            return RC ( rcVFS, rcQuery, rcExecuting, rcMessage, rcBadVersion );
    }
}


/* SOrdered */
static
rc_t SOrderedInit ( SOrdered * self, const SRaw * raw, int fields )
{
    assert ( self && raw );
    memset ( self, 0, sizeof * self );
    {
        const char * str = raw -> s;
        size_t size = string_size ( str );
        while ( size > 0 ) {
            size_t len = 0;
            char * n = string_chr ( str, size, '|' );
            if ( n != NULL )
                len = n - str;
            else
                len = size;
            if ( self -> n >= fields ) {
                return RC ( rcVFS, rcQuery, rcResolving, rcName, rcExcessive );
            }
            else {
                String * s = & self -> s [ self -> n ++ ];
                StringInit ( s, str, len, len );
                if ( n != NULL )
                    ++ len;
                str += len;
                if ( size >= len )
                    size -= len;
                else
                    size = 0;
            }
        }
    }
    return 0;
}

typedef struct {
    const char * text;
    VRemoteProtocols protocol;
} SProtocol;
static VRemoteProtocols SProtocolGet ( const String * url ) {
    size_t i = 0;
    SProtocol protocols [] = {
        { "http:", eProtocolHttp },
        { "fasp:", eProtocolFasp },
        { "https:",eProtocolHttps},
        { "file:", eProtocolFile },
        { "s3:"  , eProtocolS3   },
    };
    if ( url == NULL || url -> addr == NULL || url -> size == 0 ) {
        return eProtocolNone;
    }
    for ( i = 0; i < sizeof protocols / sizeof protocols [ 0 ]; ++ i ) {
        uint32_t sz = string_measure ( protocols [ i ] . text, NULL );
        if ( string_cmp ( url -> addr, sz, protocols [ i ] . text, sz,
            ( uint32_t ) sz ) == 0 )
        {
            return protocols [ i ] . protocol;
        }
    }
    return eProtocolNone;
}

/* STyped */
static rc_t STypedInitUrls ( STyped * self ) {
    VRemoteProtocols protocol = eProtocolNone;
    String * str = NULL;
    String * dst = NULL;
    assert ( self );
    str = & self -> url;
    while ( str -> size > 0 ) {
        size_t len = 0;
        char * n = string_chr ( str -> addr, str -> size, '$' );
        if ( n != NULL ) {
            len = n - str -> addr;
        }
        else {
            len = str -> size;
        }
        protocol = SProtocolGet ( str );
        switch ( protocol ) {
            case eProtocolFasp:
                dst = & self -> fpUrl;
                break;
            case eProtocolFile:
                dst = & self -> flUrl;
                break;
            case eProtocolHttp:
                dst = & self -> hUrl;
                break;
            case eProtocolHttps:
                dst = & self -> hsUrl;
                break;
            case eProtocolS3:
                dst = & self -> s3Url;
                break;
            default:
                return RC ( rcVFS, rcQuery, rcResolving, rcMessage, rcCorrupt );
        }
        StringInit ( dst, str -> addr, len, len );
        if ( n != NULL )
            ++ len;
        str -> addr += len;
        if ( str -> size >= len )
            str -> size -= len;
        else
            str -> size = 0;
    }
    str = & self -> vdbcacheUrl;
    while ( str -> size > 0 ) {
        size_t len = 0;
        char * n = string_chr ( str -> addr, str -> size, '$' );
        if ( n != NULL ) {
            len = n - str -> addr;
        }
        else {
            len = str -> size;
        }
        protocol = SProtocolGet ( str );
        switch ( protocol ) {
            case eProtocolFasp:
                dst = & self -> fpVdbcacheUrl;
                break;
            case eProtocolFile:
                dst = & self -> flVdbcacheUrl;
                break;
            case eProtocolHttp:
                dst = & self -> hVdbcacheUrl;
                break;
            case eProtocolHttps:
                dst = & self -> hsVdbcacheUrl;
                break;
            case eProtocolS3:
                dst = & self -> s3VdbcacheUrl;
                break;
            default:
                return RC ( rcVFS, rcQuery, rcResolving, rcMessage, rcCorrupt );
        }
        StringInit ( dst, str -> addr, len, len );
        if ( n != NULL )
            ++ len;
        str -> addr += len;
        if ( str -> size >= len )
            str -> size -= len;
        else
            str -> size = 0;
    }
    return 0;
}

static
rc_t STypedInit ( STyped * self, const SOrdered * raw, const SConverters * how,
    const SVersion * version )
{
    rc_t rc = 0;
    int i = 0;
    assert ( self && raw && how && version );
    memset ( self, 0, sizeof * self );
    if ( raw -> n != how -> n ) {
        return RC ( rcVFS, rcQuery, rcResolving, rcName, rcUnexpected );
    }
    for ( i = 0; i < raw -> n; ++i ) {
        void * dest = how -> get ( self, i );
        if ( dest == NULL ) {
            rc = RC ( rcVFS, rcQuery, rcResolving, rcMessage, rcCorrupt );
            break;
        }
        TConverter * f = how -> f [ i ];
        if ( f == NULL ) {
            rc = RC ( rcVFS, rcQuery, rcResolving, rcFunction, rcNotFound );
            break;
        }
        rc = f ( dest, & raw -> s [ i ] );
        if ( rc != 0 ) {
            break;
        }
    }
    if ( rc == 0 ) {
        if ( version -> version >= VERSION_3_2 ) {
            rc = STypedInitUrls ( self );
        }
    }

    if ( rc == 0 ) {
        self -> inited = true;
    }
    return rc;
}


/* EVPath */
static bool VPathMakeOrNot ( VPath ** new_path, const String * src,
    const String * ticket, const STyped * typed, bool ext, rc_t * rc )
{
    String bug;
    memset ( & bug, 0, sizeof bug );
    assert ( new_path && src && typed && rc );
    if ( * rc != 0 || src -> len == 0 ) {
        return false;
    }
    else {
        const String * id = & typed -> objectId;
        if ( id -> size == 0 ) {
            id = & typed -> accession;
        }
        if ( id -> size == 0 ) {
/* compensate current names.cgi-v3.0 bug: it does not return id for object-id-s
 */
            if ( src -> size > 0 &&
                 isdigit ( src -> addr [ src -> size - 1 ] ) )
            {
                size_t s = 2;
                bug . addr = src -> addr + src -> size - 1;
                bug . size = 1;
                for ( s = 2; s <= src -> size
                             && isdigit ( src -> addr [ src -> size - s ] );
                    ++ s )
                {
                    -- bug . addr;
                    ++ bug . size;
                }
                bug . len = bug . size;
                id = & bug;
            }
        }
        if ( src -> size == 0 ) {
            assert ( src -> addr != NULL );
        }
        * rc = VPathMakeFromUrl
            ( new_path, src, ticket, ext, id, typed -> size, typed -> date );
        if ( * rc == 0 ) {
            VPathMarkHighReliability ( * new_path, true );
        }
        return true;
    }
}

static rc_t EVPathInitMapping
    ( EVPath * self, const STyped * src, const SVersion * version )
{
    rc_t rc = 0;
    const VPath * vsrc = NULL;
    assert ( self && src && version );
    if ( self -> http == NULL && self -> fasp == NULL ) {
        return 0;
    }
    vsrc = self -> http ? self -> http : self -> fasp;
    rc = VPathCheckFromNamesCGI ( vsrc, & src -> ticket,
        ( const struct VPath ** ) ( & self -> mapping ) );
    if ( rc == 0) {
        if ( version -> version < VERSION_3_0 ) {
            if ( src -> ticket . size != 0 ) {
                if ( src -> accession . size != 0 ) {
                    rc = VPathMakeFmt ( & self -> mapping, "ncbi-acc:%S?tic=%S",
                        & src -> accession, & src -> ticket );
                } else if ( src -> name . size == 0 ) {
                    return 0;
                } else {
                    rc = VPathMakeFmt ( & self -> mapping,
                        "ncbi-file:%S?tic=%S", & src -> name, & src -> ticket );
                }
            }
            else if ( src -> accession . size != 0 ) {
                rc = VPathMakeFmt
                    ( & self -> mapping, "ncbi-acc:%S", & src -> accession );
            }
            else if ( src -> name . size == 0 ) {
                return 0;
            }
            else {
                rc = VPathMakeFmt
                    ( & self -> mapping, "ncbi-file:%S", & src -> name );
            }
        }
        else {
            if ( src -> ticket . size != 0 ) {
                if ( src -> objectId . size != 0 &&
                     src -> objectType == eOT_sragap )
                {
                    rc = VPathMakeFmt ( & self -> mapping, "ncbi-acc:%S?tic=%S",
                        & src -> objectId, & src -> ticket );
                }
                else {
                    if ( src -> objectId . size == 0) {
                        return 0;
                    }
                    else {
                        rc = VPathMakeFmt ( & self -> mapping,
                            "ncbi-file:%S?tic=%S",
                            & src -> objectId, & src -> ticket );
                    }
                }
            }
            else
            if ( src -> objectId . size != 0 &&
                 src -> objectType == eOT_sragap )
            {
                rc = VPathMakeFmt
                    ( & self -> mapping, "ncbi-acc:%S", & src -> objectId );
            }
            else {
                if ( src -> objectId . size == 0 ) {
                    return 0;
                }
                else {
                    rc = VPathMakeFmt (
                        & self -> mapping, "ncbi-file:%S", & src -> objectId );
                }
            }
        }
        if ( rc == 0 ) {
            return 0;
        }
        RELEASE ( VPath, self -> http );
        RELEASE ( VPath, self -> fasp );
    }
    return rc;
}

static rc_t EVPathInit ( EVPath * self, const STyped * src, 
    const SRequest * req, rc_t * r, const char * acc )
{
    rc_t rc = 0;
    bool made = false;
    KLogLevel lvl = klogInt;
    assert ( self && src && r );
    switch ( src -> code / 100 ) {
      case 0:
        rc = RC ( rcVFS, rcQuery, rcResolving, rcMessage, rcCorrupt );
        break;
      case 1:
        /* informational response
           not much we can do here */
        rc = RC ( rcVFS, rcQuery, rcResolving, rcError, rcUnexpected );
        break;
      case 2:
        /* successful response
           but can only handle 200 */
        if ( src -> code == 200 ) {
            bool ext = true;
            assert ( req );
            if ( req -> serviceType == eSTnames &&
                 req -> version .version == VERSION_1_0 )
            {
                ext = false;
            }
            made |= VPathMakeOrNot ( & self -> http,
                & src -> hUrl , & src -> ticket, src, ext, & rc );
            made |= VPathMakeOrNot ( & self -> fasp,
                & src -> fpUrl, & src -> ticket, src, ext, & rc );
            made |= VPathMakeOrNot ( & self -> https,
                & src -> hsUrl, & src -> ticket, src, ext, & rc );
            made |= VPathMakeOrNot ( & self -> file,
                & src -> flUrl, & src -> ticket, src, ext, & rc );
            made |= VPathMakeOrNot ( & self -> s3,
                & src -> s3Url, & src -> ticket, src, ext, & rc );
            VPathMakeOrNot ( & self -> vcHttp,
                & src -> hVdbcacheUrl, & src -> ticket, src, ext, & rc );
            VPathMakeOrNot ( & self -> vcFasp,
                & src -> fpVdbcacheUrl, & src -> ticket, src, ext, & rc );
            VPathMakeOrNot ( & self -> vcHttps,
                & src -> hsVdbcacheUrl, & src -> ticket, src, ext, & rc );
            VPathMakeOrNot ( & self -> vcFile,
                & src -> flVdbcacheUrl, & src -> ticket, src, ext, & rc );
            VPathMakeOrNot ( & self -> vcS3,
                & src -> s3VdbcacheUrl, & src -> ticket, src, ext, & rc );
            if ( rc == 0 ) {
                rc = EVPathInitMapping ( self, src, & req -> version );
            }
            return rc;
        }
        rc = RC ( rcVFS, rcQuery, rcResolving, rcError, rcUnexpected );
        break;
      case 3:
        /* redirection
           currently this is being handled by our request object */
        rc = RC ( rcVFS, rcQuery, rcResolving, rcError, rcUnexpected );
        break;
      case 4:
        /* client error */
        lvl = klogErr;
        switch ( src -> code )
        {
        case 400:
            rc = RC ( rcVFS, rcQuery, rcResolving, rcMessage, rcInvalid );
            break;
        case 401:
        case 403:
            rc = RC ( rcVFS, rcQuery, rcResolving, rcQuery, rcUnauthorized );
            break;
        case 404: /* 404|no data :
          If it is a real response then this assession is not found.
          What if it is a DB failure? Will be retried if configured to do so? */
            return RC ( rcVFS, rcQuery, rcResolving, rcName, rcNotFound );
        case 410:
            rc = RC ( rcVFS, rcQuery, rcResolving, rcName, rcNotFound );
            break;
        default:
            rc = RC ( rcVFS, rcQuery, rcResolving, rcError, rcUnexpected );
        }
        break;
      case 5:
        /* server error */
        lvl = klogSys;
        switch ( src -> code )
        {
        case 503:
            rc = RC ( rcVFS, rcQuery, rcResolving, rcDatabase, rcNotAvailable );
            break;
        case 504:
            rc = RC ( rcVFS, rcQuery, rcResolving, rcTimeout, rcExhausted );
            break;
        default:
            rc = RC ( rcVFS, rcQuery, rcResolving, rcError, rcUnexpected );
        }
        break;
      default:
        rc = RC ( rcVFS, rcQuery, rcResolving, rcError, rcUnexpected );
    }
    /* log message to user */
    if ( req -> errorsToIgnore == 0 ) {
        PLOGERR ( lvl, ( lvl, rc,
            "failed to resolve accession '$(acc)' - $(msg) ( $(code) )",
            "acc=%s,msg=%S,code=%u", acc, & src -> message, src -> code ) );
    }
    else {
        -- ( ( SRequest * ) req ) -> errorsToIgnore;
    }
    return rc;
}

static rc_t EVPathFini ( EVPath * self ) {
    rc_t rc = 0;
    assert ( self );
    RELEASE ( VPath, self -> mapping );
    RELEASE ( VPath, self ->   http  );
    RELEASE ( VPath, self ->   fasp  );
    RELEASE ( VPath, self ->   https );
    RELEASE ( VPath, self ->   file  );
    RELEASE ( VPath, self -> vcHttp  );
    RELEASE ( VPath, self -> vcFasp  );
    RELEASE ( VPath, self -> vcHttps );
    RELEASE ( VPath, self -> vcFile  );
    return rc;
}


/* SRow */
static rc_t SRowWhack ( void * p ) {
    rc_t rc = 0;
    rc_t r2 = 0;
    if ( p != NULL ) {
        SRow * self = ( SRow * ) p;
        r2 = EVPathFini ( & self -> path );
        if ( rc == 0) {
            rc = r2;
        }
        r2 = SRawFini ( & self -> raw );
        if ( rc == 0) {
            rc = r2;
        }
        memset ( self, 0, sizeof * self );
        free ( self );
    }
    return rc;
}

static rc_t SRowMake ( SRow ** self, char * src, const SRequest * req,
    const SConverters * f, const SVersion * version )
{
    rc_t rc = 0;
    rc_t r2 = 0;
    SRow * p = ( SRow * ) calloc ( 1, sizeof * p );
    if ( p == NULL ) {
        return RC ( rcVFS, rcQuery, rcExecuting, rcMemory, rcExhausted );
    }
    assert ( req && version );
    SRawInit ( & p -> raw, src );
    if ( rc == 0 ) {
        assert ( f );
        rc = SOrderedInit ( & p -> ordered, & p -> raw, f -> n );
    }
    if ( rc == 0 ) {
        rc = STypedInit ( & p -> typed, & p -> ordered, f, version );
    }
    if ( rc == 0 &&
         p -> typed . code == 200 && req -> request . objects == 1 )
    {
        String acc;
        size_t l
            = string_measure ( req -> request . object [ 0 ] . objectId, NULL );
        StringInit ( & acc, req -> request . object [ 0 ] . objectId, l, l );
        if ( ! StringEqual ( & p -> typed . accession, & acc ) &&
             ! StringEqual ( & p -> typed . objectId , & acc ) )
        {
            return RC ( rcVFS, rcQuery, rcResolving, rcMessage, rcCorrupt );
        }
    }
    if ( rc == 0 ) {
        const char * acc = NULL;
        if ( req -> request . objects == 1 ) {
            acc = req -> request . object [ 0 ] . objectId;
        }
        rc = EVPathInit ( & p -> path, & p -> typed, req, & r2, acc );
        if ( rc == 0 ) {
            rc = r2;
        }
    }

/* compare ticket
       currently this makes sense with 1 request from a known workspace *
    if ( download_ticket . size != 0 )
    {
        if ( ticket == NULL || ! StringEqual ( & download_ticket, ticket ) )
            return RC ( rcVFS, rcQuery, rcResolving, rcMessage, rcCorrupt );
    }
*/

    if ( rc == 0 ) {
        rc = VPathSetMake
            ( & p -> set, & p -> path, version -> version < VERSION_3_1 );
    }
    if ( rc == 0 ) {
        assert ( self );
        * self = p;
    }
    else {
        SRowWhack ( p );
    }
    return rc;
}

static void whackSRow ( void * self, void * ignore ) {
    SRowWhack ( self);
}

static void whackKartItem ( void * self, void * ignore ) {
    KartItemRelease ( ( KartItem * ) self);
}

/*static rc_t SRowUpdate ( SRow * self, const SRow * row ) {
    return 0;
}*/


/* SResponse */
static rc_t SResponseInit ( SResponse * self ) {
    rc_t rc = 0;
    assert ( self );
    VectorInit ( & self -> rows, 0, 5 );
    rc = KSrvResponseMake ( & self -> list );
    return rc;
}

static rc_t SResponseFini ( SResponse * self ) {
    rc_t rc = 0;
    rc_t r2 = 0;
    assert ( self );

    {
        void ( CC * whack ) ( void *item, void *data ) = NULL;
        if ( self -> serviceType == eSTsearch ) {
            whack = whackKartItem;
        }
        else {
            whack = whackSRow;
        }
        assert ( whack );
        VectorWhack ( & self -> rows, whack, NULL );
    }

    rc = SHeaderFini ( & self -> header );
    r2 = KSrvResponseRelease ( self -> list );
    if ( rc == 0 ) {
        rc = r2;
    }
    r2 = KartRelease ( self -> kart );
    if ( rc == 0 ) {
        rc = r2;
    }
    memset ( self, 0, sizeof * self );
    return rc;
}

static rc_t SResponseGetResponse
    ( const SResponse * self, const KSrvResponse ** response )
{
    rc_t rc = 0;
    assert ( self );
    rc = KSrvResponseAddRef ( self -> list );
    if ( rc == 0 ) {
        * response = self -> list;
    }
    return rc;
}


/* SKV */
static
rc_t SKVMake ( const SKV ** self, const char * k, const char * v )
{
    assert ( self );
    * self = NULL;
    if ( k == NULL || * k == '\0' ) {
        return RC ( rcVFS, rcQuery, rcExecuting, rcParam, rcNull );
    }
    else {
        rc_t rc = 0;
        size_t num_writ = 0;
        size_t sk = string_size ( k );
        size_t sv = string_size ( v );
        size_t s  = sk + sv + 2;
        char * p = ( char * ) malloc ( s );
        if ( p == NULL ) {
            return RC ( rcVFS, rcQuery, rcExecuting, rcMemory, rcExhausted );
        }
        rc = string_printf ( p, s, & num_writ, "%s=%s", k, v );
        assert ( num_writ <= s );
        if ( rc != 0 ) {
            free ( p );
            p = NULL;
        }
        else {
            assert ( sk );
            SKV * kv = ( SKV * ) malloc ( sizeof * kv );
            if ( kv == NULL ) {
                free ( p );
                p = NULL;
                rc = RC ( rcVFS, rcQuery, rcExecuting, rcMemory, rcExhausted );
            }
            else {
                StringInit ( & kv -> k, p, sk, sk );
                StringInit ( & kv -> v, p + sk + 1, sv, sv );
                * self = kv;
            }
        }
        return rc;
    }
}

static
rc_t SKVMakeObj ( const SKV ** self, const SObject * obj,
    ver_t version )
{
    bool old = version <= VERSION_3_0;
    char * p = NULL;
    const char * k = "object";
    if ( old ) {
        k = "acc";
    }
    char tmp [] = "";
    size_t sk = string_size ( k );
    rc_t rc = 0;
    size_t num_writ = 0;
    assert ( self && obj );
    * self = NULL;
    if ( old ) {
        rc = string_printf ( tmp, 1, & num_writ, "%s=%s", k,
            obj -> objectId );
    }
    else {
        string_printf ( tmp, 1, & num_writ, "%s=%s|%s", k,
            ObjectTypeToString ( obj -> objectType ), obj -> objectId );
    }
    ++ num_writ;
    p = ( char * ) malloc ( num_writ );
    if ( p == NULL ) {
        return RC ( rcVFS, rcQuery, rcExecuting, rcMemory, rcExhausted );
    }
    if ( old ) {
        rc = string_printf ( p, num_writ, & num_writ, "%s=%s", k,
            obj -> objectId );
    }
    else {
        rc = string_printf ( p, num_writ, & num_writ, "%s=%s|%s", k,
            ObjectTypeToString ( obj -> objectType ), obj -> objectId );
    }
    if ( rc != 0 ) {
        free ( p );
        p = NULL;
    }
    else {
        assert ( sk );
        SKV * kv = ( SKV * ) malloc ( sizeof * kv );
        if ( kv == NULL ) {
            free ( p );
            p = NULL;
            rc = RC ( rcVFS, rcQuery, rcExecuting, rcMemory, rcExhausted );
        }
        else {
            -- num_writ;
            StringInit ( & kv -> k, p, sk, sk );
            StringInit ( & kv -> v, p + sk + 1, num_writ, num_writ );
            * self = kv;
        }
    }
    return rc;
}


typedef struct {
    Vector * v;
    rc_t   rc;
} Tickets;

static void TicketsAppendTicket ( void * item, void * data ) {
    const String * ticket = ( String    * ) item;
    Tickets    * v      = ( Tickets * ) data;
    const SKV * kv = NULL;
    const char k [] = "tic";
    char * c = string_dup ( ticket -> addr, ticket -> size );
    if ( c == NULL ) {
        v -> rc = RC ( rcVFS, rcQuery, rcExecuting, rcMemory, rcExhausted );
        return;
    }
    DBGMSG ( DBG_VFS, DBG_FLAG ( DBG_VFS_SERVICE ), ( "  %s=%s\n",
        k, c ) );
    rc_t rc = SKVMake ( & kv, k, c );
    free ( c );
    if ( rc == 0 ) {
        rc_t rc = VectorAppend ( v -> v, NULL, kv );
        if ( rc != 0 && v -> rc == 0) {
            v -> rc = rc;
        }
    }
}


static void whackSKV ( void * p, void * ignore ) {
    SKV * self = ( SKV * ) p;
    assert ( self );
    free ( ( void * ) self -> k . addr );
    memset ( self, 0, sizeof * self );
    free ( self );
}


/* SHttpRequestHelper */
static rc_t SHttpRequestHelperInit ( SHttpRequestHelper * self,
    const KNSManager * mgr, const char * cgi )
{
    rc_t rc = 0;
    assert ( self );
    memset ( self, 0, sizeof * self );
    if ( rc == 0 ) {
        rc = KNSManagerMakeReliableClientRequest
            ( mgr, & self -> req, 0x01010000, NULL, cgi );
    }
    return rc;
}

static rc_t SHttpRequestHelperFini ( SHttpRequestHelper * self ) {
    rc_t rc = 0;
    assert ( self );
    RELEASE ( KHttpRequest, self -> req );
    return rc;
}

static
void SHttpRequestHelperAddPostParam ( void * item, void * data )
{
    const SKV          * kv = ( SKV                * ) item;
    SHttpRequestHelper * p  = ( SHttpRequestHelper * ) data;
    rc_t rc = 0;
    assert ( kv && p );
    rc = KHttpRequestAddPostParam ( p -> req, kv -> k . addr );
    if ( p -> rc == 0 ) {
        p -> rc = rc;
    }
}


/* SCgiRequest */
static
rc_t SCgiRequestInitCgi ( SCgiRequest * self, const char * cgi )
{
    assert ( self && ! self -> cgi );
    self -> cgi = string_dup_measure ( cgi, NULL );
    if ( self -> cgi == NULL ) {
        return RC ( rcVFS, rcQuery, rcExecuting, rcParam, rcNull );
    }
    return 0;
}

static
rc_t ECgiNamesRequestInit ( SRequest * request, SHelper * helper,
    VRemoteProtocols protocols, const char * cgi,
    const char * version )
{
    SCgiRequest * self = NULL;
    rc_t rc = 0;
    const SKV * kv = NULL;
    assert ( request );
    rc = SVersionInit ( & request -> version, version, eSTnames );
    if ( rc != 0 )
        return rc;
    if ( protocols == eProtocolDefault ) {
/* get from kfg, otherwise use hardcoded below */
        protocols = SHelperDefaultProtocols ( helper );
    }
    request -> protocols = protocols;
    self = & request -> cgiReq;
    if ( self -> cgi == NULL ) {
        if ( cgi == NULL ) {
/* try to get cgi from kfg, otherwise use hardcoded below */
            cgi =  "http://www.ncbi.nlm.nih.gov/Traces/names/names.cgi";
            cgi = "https://www.ncbi.nlm.nih.gov/Traces/names/names.cgi";
        }
        rc = SCgiRequestInitCgi ( self, cgi );
    }
    VectorInit ( & self -> params, 0, 5 );
    request -> serviceType = eSTnames;
    DBGMSG ( DBG_VFS,
        DBG_FLAG ( DBG_VFS_SERVICE ), ( "CGI = '%s'\n", self -> cgi ) );
    if ( rc == 0 ) {
        const char name [] = "version";
        char * version = NULL;
        rc = SVersionToString ( & request -> version, & version );
        if ( rc != 0 ) {
            return rc;
        }
        rc = SKVMake ( & kv, name, version );
        if ( rc == 0 ) {
            rc = VectorAppend ( & self -> params, NULL, kv );
            DBGMSG ( DBG_VFS, DBG_FLAG ( DBG_VFS_SERVICE ),
                ( "  %s=%s\n", name, version ) );
        }
        free ( version );
        if ( rc != 0 ) {
            return rc;
        }
    }
    if ( request -> version . version < VERSION_3_0 ) {
        if ( request -> request . object [ 0 ] . objectId == NULL ) {
            return RC ( rcVFS, rcQuery, rcExecuting, rcParam, rcNull );
        }
        else {
            const char name [] = "acc";
            rc = SKVMake
                ( & kv, name, request -> request . object [ 0 ] . objectId );
            DBGMSG ( DBG_VFS, DBG_FLAG ( DBG_VFS_SERVICE ), ( "  %s=%s\n",
                name, request -> request . object [ 0 ] . objectId ) );
            if ( rc == 0 ) {
                rc = VectorAppend ( & self -> params, NULL, kv );
            }
        }
        if ( rc != 0 ) {
            return rc;
        }
    }
    else {
        uint32_t i = 0;
        for ( i = 0; i < request -> request . objects; ++i ) {
            rc = SKVMakeObj ( & kv, & request -> request . object [ i ],
                request -> version . version );
            if ( rc == 0 ) {
                DBGMSG ( DBG_VFS, DBG_FLAG ( DBG_VFS_SERVICE ),
                    ( "  %.*s=%.*s\n", kv -> k . len, kv -> k . addr,
                                           kv -> v . len, kv -> v . addr ) );
                rc = VectorAppend ( & self -> params, NULL, kv );
            }
        }
        if ( rc != 0 ) {
            return rc;
        }
    }
    if ( request -> tickets . size != 0 ) { /* optional */
        Tickets t = { & self -> params, 0 };
        VectorForEach ( & request -> tickets .tickets , false,
            TicketsAppendTicket, & t );
        rc = t . rc;
        if ( rc != 0 ) {
            return rc;
        }
    }
    {
        uint32_t i = 0;
        const char * prefs [ eProtocolMaxPref ];
        const char * seps [ eProtocolMaxPref ];
        VRemoteProtocols protos = protocols;

        prefs [ 0 ] = seps [ 0 ] = NULL;
        prefs [ 1 ] = seps [ 1 ] = prefs [ 2 ] = seps [ 2 ]
                                 = prefs [ 3 ] = seps [ 3 ] = "";

        for ( i = 0; protos != 0 && i < sizeof prefs / sizeof prefs [ 0 ];
            protos >>= 3 )
        {
            /* 1.1 protocols */
            switch ( protos & eProtocolMask )
            {
            case eProtocolHttp:
                prefs [ i ] = "http";
                seps [ i ++ ] = ",";
                break;
            case eProtocolFasp:
                prefs [ i ] = "fasp";
                seps [ i ++ ] = ",";
                break;
            /* 1.2 protocols */
            case eProtocolHttps:
                prefs [ i ] = "https";
                seps [ i ++ ] = ",";
                break;
            /* 3.1 protocols */
            case eProtocolFile:
                prefs [ i ] = "file";
                seps [ i ++ ] = ",";
                break;
            default:
                assert ( 0 );
                break;
            }
        }
        if ( prefs [ 0 ] == NULL )
            rc = RC ( rcVFS, rcQuery, rcResolving, rcParam, rcInvalid );
        else
        {
            const char name [] = "accept-proto";
            size_t num_writ = 0;
            char p [ 512 ] = "";
            rc = string_printf ( p, sizeof p, & num_writ, "%s%s%s%s%s%s",
                prefs [ 0 ], seps [ 1 ], prefs [ 1 ], seps [ 2 ], prefs [ 2 ],
                                                      seps [ 3 ], prefs [ 3 ] );
            rc = SKVMake ( & kv, name, p );
            if ( rc == 0 ) {
                DBGMSG ( DBG_VFS, DBG_FLAG ( DBG_VFS_SERVICE ),
                    ( "  %s=%s\n", name, p ) );
                rc = VectorAppend ( & self -> params, NULL, kv );
            }
        }
        if ( rc != 0 ) {
            return rc;
        }
    }
    if ( request -> version . version < VERSION_3_0 &&
         request -> request . refseq_ctx )
    {
        const char name [] = "ctx";
        rc = SKVMake ( & kv, name, "refseq" );
        if ( rc == 0 ) {
                DBGMSG ( DBG_VFS, DBG_FLAG ( DBG_VFS_SERVICE ),
                    ( "  %s=refseq\n", name ) );
            rc = VectorAppend ( & self -> params, NULL, kv );
        }
        if ( rc != 0 ) {
            return rc;
        }
    }
    if ( request -> version . version == VERSION_3_0 ) {
        if ( request -> request . object [ 0 ] . objectType !=
            eOT_undefined )
        {
            const char name [] = "typ";
            const char * v = ObjectTypeToString
                ( request -> request . object [ 0 ] . objectType );
            rc = SKVMake ( & kv, name, v );
            if ( rc == 0 ) {
                DBGMSG ( DBG_VFS, DBG_FLAG ( DBG_VFS_SERVICE ),
                    ( "  %s=%s\n", name, v ) );
                rc = VectorAppend ( & self -> params, NULL, kv );
            }
        }
        if ( rc != 0 ) {
            return rc;
        }
    }
    return rc;
}


static
rc_t ECgiSearchRequestInit ( SRequest * request, const char * cgi,
    const char * version )
{
    SCgiRequest * self = NULL;
    rc_t rc = 0;
    const SKV * kv = NULL;
    assert ( request );
    rc = SVersionInit ( & request -> version, version, eSTnames );
    if ( rc != 0 )
        return rc;
    self = & request -> cgiReq;
    if ( self -> cgi == NULL ) {
        if ( cgi == NULL ) {
/* try to get cgi from kfg, otherwise use hardcoded below */
            cgi =  "http://www.ncbi.nlm.nih.gov/Traces/names/search.cgi";
            cgi = "https://www.ncbi.nlm.nih.gov/Traces/names/search.cgi";
        }
        rc = SCgiRequestInitCgi ( self, cgi );
    }
    request -> serviceType = eSTsearch;
    VectorInit ( & self -> params, 0, 5 );
    DBGMSG ( DBG_VFS,
        DBG_FLAG ( DBG_VFS_SERVICE ), ( "CGI = '%s'\n", self -> cgi ) );
    if ( rc == 0 ) {
        const char name [] = "version";
        char * version = NULL;
        rc = SVersionToString ( & request -> version, & version );
        if ( rc != 0 ) {
            return rc;
        }
        rc = SKVMake ( & kv, name, version );
        if ( rc == 0 ) {
            rc = VectorAppend ( & self -> params, NULL, kv );
            DBGMSG ( DBG_VFS, DBG_FLAG ( DBG_VFS_SERVICE ),
                ( "  %s=%s\n", name, version ) );
        }
        free ( version );
        if ( rc != 0 ) {
            return rc;
        }
    }
    {
        const char name [] = "term";
        char * b = NULL;
        uint32_t i = 0;
        size_t l = 0;
        size_t o = 0;
        for ( i = 0; i < request -> request . objects; ++i ) {
            l += string_measure
                ( request -> request . object [ i ] . objectId, NULL ) + 1;
        }
        if ( l > 0 ) {
            b = ( char * ) malloc ( l );
            if ( b == NULL ) {
                return
                    RC ( rcVFS, rcQuery, rcExecuting, rcMemory, rcExhausted );
            }
            for ( i = 0; rc == 0 && i < request -> request . objects;
                ++i )
            {
                size_t num_writ = 0;
                rc = string_printf ( b + o, l - o, & num_writ,
                    "%s", request -> request . object [ i ] );
                o += num_writ;
                if ( i + 1 == request -> request . objects ) {
                    b [ o ++ ] = '\0';
                }
                else {
                    b [ o ++ ] = ',';
                }
            }
            assert ( o <= l );
            rc = SKVMake ( & kv, name, b );
            DBGMSG ( DBG_VFS, DBG_FLAG ( DBG_VFS_SERVICE ),
                ( "  %s=%s\n", name, b ) );
            if ( rc == 0 ) {
                rc = VectorAppend ( & self -> params, NULL, kv );
            }
            free ( b );
            if ( rc != 0 ) {
                return rc;
            }
        }
    }
    return rc;
}


static void SCgiRequestFini ( SCgiRequest * self ) {
    assert ( self );
    free ( self -> cgi );
    VectorWhack ( & self -> params, whackSKV, NULL );
    memset ( self, 0, sizeof * self );
}

static rc_t SCgiRequestPerform ( const SCgiRequest * self,
    const SHelper * helper, KStream ** response )
{
    rc_t rc = 0;
    assert ( self && helper );
    if ( rc == 0 ) {
        SHttpRequestHelper h;
        rc = SHttpRequestHelperInit ( & h, helper -> kMgr, self-> cgi );
        if ( rc == 0 ) {
            VectorForEach (
                & self -> params, false, SHttpRequestHelperAddPostParam, & h );
            rc = h . rc;
        }
        if ( rc == 0 ) {
            KHttpResult * rslt = NULL;
            rc = KHttpRequestPOST ( h . req, & rslt );
            if ( rc == 0 ) {
                uint32_t code = 0;
                rc = KHttpResultStatus ( rslt, & code, NULL, 0, NULL );
                if ( rc == 0 ) {
                    if ( code == 200 ) {
                        rc = KHttpResultGetInputStream ( rslt, response );
                    }
                    else {
                        rc = RC ( rcVFS,
                            rcQuery, rcExecuting, rcConnection, rcUnexpected );
                    }
                }
            }
            RELEASE ( KHttpResult, rslt );
        }
        {
            rc_t r2 = SHttpRequestHelperFini ( & h );
            if ( rc == 0 ) {
                rc = r2;
            }
        }
    }
    return rc;
}


/* SObject */
static rc_t SObjectInit ( SObject * self,
    const char * objectId, size_t objSz, EObjectType objectType )
{
    assert ( self );
    self -> objectType = objectType;
    if ( objectId != NULL && objSz != 0 ) {
        self -> objectId = string_dup ( objectId, objSz );
        if ( self -> objectId == NULL ) {
            return RC ( rcVFS, rcQuery, rcExecuting, rcMemory, rcExhausted );
        }
    }
    return 0;
}

static void SObjectFini ( SObject * self ) {
    assert ( self );
    free ( self -> objectId );
    memset ( self, 0, sizeof * self );
}


/* STickets */
const uint64_t BICKETS = 1024;
static rc_t STicketsAppend ( STickets * self, const char * ticket ) {
    rc_t rc = 0;
    const char * comma = "";
    assert ( self );
    if ( ticket == NULL ) {
        return 0;
    }
    if ( self -> size > 0 ) {
        comma = ",";
    }
    do {
        size_t num_writ = 0;
        char * p = ( char * ) self -> str . base;
        assert ( comma );
        rc = string_printf ( p + self -> size,
            self -> str . elem_count - self -> size, & num_writ,
            "%s%s", comma, ticket );
        if ( rc == 0 ) {
            rc_t r2 = 0;
            String * s = ( String * ) malloc ( sizeof * s );
            if ( s == NULL ) {
                rc = RC ( rcVFS, rcQuery, rcExecuting, rcMemory, rcExhausted );
            } else {
                const char * addr = p + self -> size;
                uint32_t len = num_writ;
                if ( comma [ 0 ] != '\0' ) {
                    ++ addr;
                    -- len;
                }
                StringInit ( s, addr, len, len );
                r2 = VectorAppend ( & self -> tickets, NULL, s );
                if ( r2 != 0 ) {
                    rc = r2;
                    free ( s );
                }
                self -> size += num_writ;
                break;
            }
        }
        else if ( GetRCState ( rc ) == rcInsufficient
            && GetRCObject ( rc ) == ( enum RCObject ) rcBuffer )
        {
            size_t needed = BICKETS;
            if ( self -> str . elem_count - self -> size + needed < num_writ ) {
                needed = num_writ;
            }
            rc = KDataBufferResize
                ( & self -> str, self -> str . elem_count + needed );
        }
        else {
            break;
        }
    } while ( rc == 0 );
    return rc;
}

/*static void STicketsAppendFromVector ( void * item, void * data ) {
    STickets   * self   = ( STickets * ) data;
    rc_t rc = STicketsAppend ( self, ( char * ) item );
    if ( rc != 0 ) {
        self -> rc = rc;
    }
}*/

static rc_t STicketsInit ( STickets * self ) {
    assert ( self );
    memset ( self, 0, sizeof * self );
    return KDataBufferMakeBytes ( & self -> str, BICKETS );
}

static void whack_free ( void * self, void * ignore ) {
    if ( self != NULL ) {
        memset ( self, 0, sizeof ( * ( char * ) self ) );
        free ( self );
    }
}

static rc_t STicketsFini ( STickets * self ) {
    assert ( self );
    rc_t rc = KDataBufferWhack ( & self -> str );
    VectorWhack ( & self -> tickets, whack_free, NULL );
    memset ( self, 0 , sizeof * self );
    return rc;
}


/* SRequestData */
/*static
rc_t SRequestDataInit ( SRequestData * self, const char * acc, size_t acc_sz,
    EObjectType objectType )
{
    rc_t rc = 0;
    assert ( self );
    memset ( self, 0, sizeof * self );
    if ( acc != NULL && acc_sz != 0 ) {
        self -> objects = 1;
        rc = SObjectInit ( & self -> object [ 0 ], acc, acc_sz, objectType );
    }
    return rc;
}*/

static void SRequestDataFini ( SRequestData * self ) {
    uint32_t i = 0;
    assert ( self );
    for ( i = 0; i < self -> objects; ++i ) {
        SObjectFini ( & self -> object [ i ] );
    }
    memset ( self, 0, sizeof * self );
}

static
rc_t SRequestDataAppendObject ( SRequestData * self, const char * id,
    size_t id_sz, EObjectType objectType )
{
    rc_t rc = 0;
    assert ( self );
    if ( self -> objects > sizeof self -> object / sizeof self -> object [ 0 ] )
    {
        return RC ( rcVFS, rcQuery, rcExecuting, rcItem, rcExcessive );
    }
    if ( id_sz == 0 )
        id_sz = string_measure ( id, NULL );
    rc = SObjectInit ( & self -> object [ self -> objects ], id, id_sz,
        objectType );
    if ( rc == 0 ) {
        ++ self -> objects;
    }
    return rc;
}


/* SRequest */
static rc_t SRequestInit ( SRequest * self ) {
    assert ( self );

    memset ( self, 0, sizeof * self );

    return STicketsInit ( & self -> tickets );
}

static rc_t SRequestFini ( SRequest * self ) {
    rc_t rc = 0;
    rc_t r2 = 0;
    assert ( self );
    rc = STicketsFini ( & self -> tickets );
    r2 = SVersionFini ( & self -> version );
    if ( rc == 0 ) {
        rc = r2;
    }
    SRequestDataFini ( & self -> request );
    SCgiRequestFini ( & self -> cgiReq );
    memset ( self, 0, sizeof * self );
    return rc;
}

static rc_t SRequestAddTicket ( SRequest * self, const char * ticket ) {
    assert ( self );
    return STicketsAppend ( & self -> tickets, ticket );
}


/* KService */
static void KServiceExpectErrors ( KService * self, int n ) {
    assert ( self );

    self -> req . errorsToIgnore = n;
}

static rc_t KServiceAddObject ( KService * self,
    const char * id, size_t id_sz, EObjectType objectType )
{
    if ( self == NULL )
        return RC ( rcVFS, rcQuery, rcExecuting, rcParam, rcNull );

    return SRequestDataAppendObject
        ( & self -> req . request, id, id_sz, objectType );
}

rc_t KServiceAddId ( KService * self, const char * id ) {
    return KServiceAddObject ( self, id, 0, eOT_undefined );
}

static rc_t KServiceAddTicket ( KService * self, const char * ticket ) {
    if ( self == NULL )
        return RC ( rcVFS, rcQuery, rcExecuting, rcSelf, rcNull );

    if ( ticket == NULL )
        return RC ( rcVFS, rcQuery, rcExecuting, rcParam, rcNull );

    return SRequestAddTicket ( & self -> req, ticket );
}

rc_t KServiceAddProject ( KService * self, uint32_t project ) {
    rc_t rc = 0;

    char buffer [ 256 ] = "";
    size_t ticket_size = ~0;

    if ( project == 0 )
        return 0;

    if ( self == NULL )
        return RC ( rcVFS, rcQuery, rcExecuting, rcSelf, rcNull );

    rc = SHelperProjectToTicket ( & self -> helper, project,
        buffer, sizeof buffer, & ticket_size );
    if ( rc != 0 )
        return rc;

    assert ( ticket_size <= sizeof buffer );

    return SRequestAddTicket ( & self -> req, buffer );
}

static
rc_t KServiceInitNamesRequestWithVersion ( KService * self,
    VRemoteProtocols protocols,
    const char * cgi, const char * version )
{
    assert ( self );

    return ECgiNamesRequestInit ( & self -> req,  & self -> helper, protocols,
        cgi, version );
}

static
rc_t KServiceInitNamesRequest ( KService * self, VRemoteProtocols protocols,
    const char * cgi )
{
    return KServiceInitNamesRequestWithVersion ( self, protocols, cgi, "#3.0" );
}

static
rc_t KServiceInitSearchRequestWithVersion ( KService * self, const char * cgi,
    const char * version )
{
    assert ( self );

    return ECgiSearchRequestInit ( & self -> req, cgi, version );
}

static rc_t KServiceInit ( KService * self, const KNSManager * mgr ) {
    rc_t rc = 0;

    assert ( self );
    memset ( self, 0, sizeof * self );

    if ( rc == 0 )
        rc = SHelperInit ( & self -> helper, mgr );

    if ( rc == 0 )
        rc = SResponseInit ( & self ->  resp );

    if ( rc == 0 )
        rc = SRequestInit ( & self -> req );

    return rc;
}

static rc_t KServiceInitNames1 ( KService * self, const KNSManager * mgr,
    const char * cgi, const char * version, const char * acc,
    size_t acc_sz, const char * ticket, VRemoteProtocols protocols,
    EObjectType objectType, bool refseq_ctx )
{
    rc_t rc = 0;

    if ( rc == 0 )
        rc = KServiceInit ( self, mgr );

    if ( rc == 0 )
        rc = KServiceAddObject ( self, acc, acc_sz, objectType );
    if ( rc == 0 )
        rc = SRequestAddTicket ( & self -> req,  ticket );
    if ( rc == 0 )
        self -> req . request . refseq_ctx = refseq_ctx;

    if ( rc == 0 )
        rc = KServiceInitNamesRequestWithVersion
            ( self, protocols, cgi, version );

    return rc;
}

static
rc_t KServiceMakeWithMgr ( KService ** self, const KNSManager * mgr )
{
    rc_t rc = 0;

    KService * p = NULL;

    if ( self == NULL )
        return RC ( rcVFS, rcQuery, rcExecuting, rcParam, rcNull );

    p = ( KService * ) calloc ( 1, sizeof * p );
    if ( p == NULL )
        return RC ( rcVFS, rcQuery, rcExecuting, rcMemory, rcExhausted );

    rc = KServiceInit ( p, mgr );
    if ( rc == 0)
        * self = p;
    else
        free ( p );

    return rc;
}

rc_t KServiceMake ( KService ** self) {
    return KServiceMakeWithMgr ( self, NULL );
}

static rc_t KServiceFini ( KService * self ) {
    rc_t rc = 0;
    rc_t r2 = 0;

    assert ( self );

    r2 = SResponseFini ( & self -> resp );
    if ( rc == 0 )
        rc = r2;

    r2 = SRequestFini ( & self -> req );
    if ( rc == 0 )
        rc = r2;

    r2 = SHelperFini ( & self -> helper );
    if ( rc == 0 )
        rc = r2;

    return rc;
}

rc_t KServiceRelease ( KService * self ) {
    rc_t rc = 0;

    if ( self != NULL ) {
        rc = KServiceFini ( self );
        free ( self );
    }

    return rc;
}

static
rc_t KServiceProcessStream ( KService * self, KStream * stream )
{
    bool start = true;
    char buffer [ 4096 ] = "";
    size_t num_read = 0;
    timeout_t tm;
    rc_t rc = 0;
    size_t sizeW = sizeof buffer;
    size_t sizeR = 0;
    size_t offR = 0;
    size_t offW = 0;
    char * newline = NULL;
    assert ( self );
    self -> resp . serviceType = self -> req . serviceType;
    rc = TimeoutInit ( & tm, self -> helper . timeoutMs );
    if ( rc == 0 && self -> req . serviceType == eSTsearch ) {
        rc = KartMake2 ( & self -> resp . kart );
    }
    while ( rc == 0 ) {
        if ( sizeR == 0 ) {
            rc = KStreamTimedRead
                ( stream, buffer + offW, sizeW, & num_read, & tm );
            if ( rc != 0 || num_read == 0 ) {
                break;
            }
            DBGMSG ( DBG_VFS, DBG_FLAG ( DBG_VFS_SERVICE ),
                ( "<<<<<<<<<<\n%.*s\n", ( int ) num_read - 1, buffer + offW ) );
            sizeR += num_read;
            offW += num_read;
            if (sizeW >= num_read ) {
                sizeW -= num_read;
            }
            else {
                sizeW = 0;
            }
        }
        newline = string_chr ( buffer + offR, sizeR, '\n' );
/* TODO different conditions: move buffer content; partial read; line > buf size
 */
        if ( newline == NULL ) {
            if ( sizeW == 0 && offR == 0 ) {
                rc = RC
                    ( rcVFS, rcQuery, rcExecuting, rcString, rcInsufficient );
                break;
            }
            rc = KStreamTimedRead
                ( stream, buffer + offW, sizeW, & num_read, & tm );
            if ( rc != 0 ) {
                /* TBD - look more closely at rc */
                if ( num_read > 0 )
                    rc = 0;
                else
                    break;
            }
            else if ( num_read == 0 ) {
                rc = RC /* stream does not end by '\n' */
                    ( rcVFS, rcQuery, rcExecuting, rcString, rcInsufficient ); 
                break;
            }
            sizeR += num_read;
            offW += num_read;
            if (sizeW >= num_read ) {
                sizeW -= num_read;
            }
            else {
                sizeW = 0;
            }
        }
        else {
            size_t size = newline - ( buffer + offR );
            char * s = string_dup ( buffer + offR, size );
            if ( s == NULL ) {
                rc = RC ( rcVFS, rcQuery, rcExecuting, rcMemory, rcExhausted );
                break;
            }
            if ( start ) {
                rc = SHeaderMake
                    ( & self -> resp . header, s, self -> req . serviceType );
                if ( rc != 0 ) {
                    break;
                }
                start = false;
            }
            else {
                if ( s [ 0 ] == '$' ) {
                    free ( s );
                    s = NULL;
                    break;
                }
                else if ( self -> req . serviceType == eSTsearch ) {
                    const char end [] = "$end";
                    size_t sz = sizeof end - 1;
                    if ( string_cmp ( s, size, end, sz, ( uint32_t ) sz )
                        == 0)
                    {
                        free ( s );
                        s = NULL;
                        break;
                    }
                    else {
                        rc = KartAddRow ( self -> resp . kart, s, size );
                    }
                }
                else {
                    const SConverters * f = NULL;
                    rc = SConvertersMake ( & f, & self -> resp . header );
                    if ( rc == 0 ) {
                        SRow * row = NULL;
                        rc_t r2 = SRowMake ( & row, s, & self -> req, f,
                            & self -> resp . header . version );
                        uint32_t l = VectorLength ( & self -> resp . rows );
                        if ( r2 == 0 ) {
                            if ( self -> resp . header. version. version
                                                      >= VERSION_3_0
                                 || l == 0 )
                            {
                                r2 = VectorAppend
                                    ( & self -> resp . rows, NULL, row );
                            }
                            else {
                      /* ACC.vdbcashe : TODO : search for vdb.cache extension */
                                if ( row -> typed . objectId . len == 18 || 
                                     row -> typed . objectId . len == 19 )
                                {
                                    r2 = SRowWhack ( row );
                                    row = NULL;
                                }
                            }
                        }
                        if ( r2 == 0 ) {
                            if ( self -> resp . header . version. version
                                                          >= VERSION_3_0
                               ||
                               KSrvResponseLength ( self -> resp . list )
                                                          == 0 )
                            {
                                r2 = KSrvResponseAppend
                                    ( self -> resp . list, row -> set );
                            }
                            else {
                                assert ( ! row );
                            }
                        }
                        if ( r2 != 0 && rc == 0 && l != 1 ) {
                            rc = r2;
                        }
                    }
                }
                if ( rc != 0 ) {
                    break;
                }
            }
            ++ size;
            offR += size;
            if ( sizeR >= size ) {
                sizeR -= size;
            }
            else {
                sizeR = 0;
            }
            if ( sizeR == 0 && offR == offW ) {
                offR = offW = 0;
                sizeW = sizeof buffer;
            }
        }
    }
    if ( start ) {
        rc = RC ( rcVFS, rcQuery, rcExecuting, rcString, rcInsufficient );
    }
    DBGMSG ( DBG_VFS, DBG_FLAG ( DBG_VFS_SERVICE ), ( ">>>>>>>>>>\n\n" ) );
    return rc;
}

static
rc_t KServiceGetResponse
    ( const KService * self, const KSrvResponse ** response )
{
    if ( self == NULL ) {
        return RC ( rcVFS, rcQuery, rcExecuting, rcSelf, rcNull );
    }
    if ( response == NULL ) {
        return RC ( rcVFS, rcQuery, rcExecuting, rcParam, rcNull );
    }
    else {
        return SResponseGetResponse ( & self -> resp, response );
    }
}


rc_t KServiceNamesExecuteExt ( KService * self, VRemoteProtocols protocols,
    const char * cgi, const char * version,
    const KSrvResponse ** response )
{
    rc_t rc = 0;

    KStream * stream = NULL;

    if ( self == NULL )
         return RC ( rcVFS, rcQuery, rcExecuting, rcSelf, rcNull );

    if ( response == NULL )
         return RC ( rcVFS, rcQuery, rcExecuting, rcParam, rcNull );

    if ( version == NULL )
        version = "#3.0";

    rc = KServiceInitNamesRequestWithVersion ( self, protocols, cgi, version );

    if ( rc == 0 )
        rc = SCgiRequestPerform
            ( & self -> req . cgiReq, & self -> helper, & stream );

    if ( rc == 0 )
        rc = KServiceProcessStream ( self, stream );

    if ( rc == 0 )
        rc = KServiceGetResponse ( self, response );

    RELEASE ( KStream, stream );

    return rc;
}


rc_t KServiceNamesExecute ( KService * self, VRemoteProtocols protocols,
    const KSrvResponse ** response )
{
    return KServiceNamesExecuteExt ( self, protocols, NULL, NULL, response );
}


static rc_t CC KService1NameWithVersionAndType ( const KNSManager * mgr,
    const char * url, const char * acc, size_t acc_sz, const char * ticket,
    VRemoteProtocols protocols, const VPath ** remote,  const VPath ** mapping,
    bool refseq_ctx, const char * version, EObjectType objectType )
{
    rc_t rc = 0;

    KStream * stream = NULL;

    KService service;

    if ( acc == NULL || remote == NULL )
        return RC ( rcVFS, rcQuery, rcExecuting, rcParam, rcNull );

    rc = KServiceInitNames1 ( & service, mgr, url, version,
        acc, acc_sz, ticket, protocols, objectType, refseq_ctx );

    protocols = service . req . protocols;

    if ( rc == 0 )
        rc = SCgiRequestPerform
            ( & service . req . cgiReq, & service . helper, & stream );

    if ( rc == 0 )
        rc = KServiceProcessStream ( & service, stream );

    if ( rc == 0 ) {
        if ( VectorLength ( & service . resp . rows ) != 1)
            rc = 1;
        else {
            const SRow * r =
                ( SRow * ) VectorGet ( & service . resp . rows, 0 );
            if ( r == NULL)
                rc = 2;
            else {
                const VPath * path = r -> path . http;
                rc = VPathAddRef ( path );
                if ( rc == 0 )
                    * remote = path;
                if ( mapping ) {
                    path = r -> path . mapping;
                    rc = VPathAddRef ( path );
                    if ( rc == 0 )
                        * mapping = path;
                }
            }
        }
    }

    if ( rc == 0 ) {
        uint32_t l = KSrvResponseLength ( service . resp . list );
        if ( l != 1)
            rc = 3;
        else {
            const VPathSet * s = NULL;
            rc = KSrvResponseGet ( service . resp . list, 0, & s );
            if ( rc != 0 ) {
            }
            else if ( s == NULL )
                rc = 4;
            else {
                const VPath * path = NULL;
                const VPath * cache = NULL;
                rc = VPathSetGet ( s, protocols, & path, & cache );
                if ( rc == 0 ) {
                    int notequal = ~ 0;
                    assert ( remote );
                    rc = VPathEqual ( * remote, path, & notequal );
                    if ( rc == 0 )
                        rc = notequal;
                    RELEASE ( VPath, cache );
                    RELEASE ( VPath, path );
                }
            }
            RELEASE ( VPathSet, s );
        }
    }

    {
        rc_t r2 = KServiceFini ( & service );
        if ( rc == 0 )
            rc = r2;
    }

    RELEASE ( KStream, stream );

    return rc;
}

LIB_EXPORT
rc_t CC KService1NameWithVersion ( const KNSManager * mgr, const char * url,
    const char * acc, size_t acc_sz, const char * ticket,
    VRemoteProtocols protocols, const VPath ** remote, const VPath ** mapping,
    bool refseq_ctx, const char * version )
{
    if ( version == NULL )
        return RC ( rcVFS, rcQuery, rcExecuting, rcParam, rcNull );

    return KService1NameWithVersionAndType ( mgr, url, acc, acc_sz, ticket,
        protocols, remote, mapping, refseq_ctx, version, eOT_undefined );
}


rc_t KServiceSearchExecuteExt ( KService * self, const char * cgi,
    const char * version, const Kart ** result )
{
    rc_t rc = 0;

    KStream * stream = NULL;

    if ( self == NULL )
        return RC ( rcVFS, rcQuery, rcExecuting, rcSelf, rcNull );

    if ( result == NULL )
        return RC ( rcVFS, rcQuery, rcExecuting, rcParam, rcNull );

    if ( version == NULL )
        version = "#1.0";

    rc = KServiceInitSearchRequestWithVersion ( self, cgi, version );

    if ( rc == 0 )
        rc = SCgiRequestPerform
            ( & self -> req . cgiReq, & self -> helper, & stream );

    if ( rc == 0 )
        rc = KServiceProcessStream ( self, stream );

    if ( rc == 0 ) {
        rc = KartAddRef ( self -> resp . kart );
        if ( rc == 0 )
            * result = self -> resp . kart;
    }

    RELEASE ( KStream, stream );

    return rc;
}


rc_t KServiceSearchExecute ( KService * self, const Kart ** result ) {
    return KServiceSearchExecuteExt ( self, NULL, NULL, result );
}


rc_t KService1Search ( const KNSManager * mgr, const char * cgi,
    const char * acc, const Kart ** result )
{
    rc_t rc = 0;

    KService service;

    rc = KServiceInit ( & service, mgr );

    if ( rc == 0 )
        rc = KServiceAddId ( & service, acc );

    if ( rc == 0 )
        rc = KServiceSearchExecute ( & service, result );

    {
        rc_t r2 = KServiceFini ( & service );
        if ( rc == 0 )
            rc = r2;
    }
    return rc;
}


/* TESTS */
typedef struct {
    rc_t passed;
    const char * acc;
    const char * version;
    VRemoteProtocols protocols;
} SKVCheck;

static void SCgiRequestCheck ( void * item, void * data ) {
 /* const SKV * kv = ( SKV      * ) item; */
    SKVCheck  * p  = ( SKVCheck * ) data;
    assert ( p );
    p -> passed = 0;
}

static void SKVCheckInit ( SKVCheck * self, const char * acc,
    const char * version, VRemoteProtocols protocols )
{
    assert ( self );
    memset ( self, 0, sizeof * self );
    self -> acc = acc;
    self -> version = version;
    self -> protocols = protocols;
    self -> passed = -1;
}

rc_t KServiceRequestTestNames1 ( const KNSManager * mgr,
    const char * cgi, const char * version, const char * acc, size_t acc_sz,
    const char * ticket, VRemoteProtocols protocols,
    EObjectType objectType )
{
    KService service;
    rc_t rc = KServiceInitNames1 ( & service, mgr, cgi, version,
        acc, acc_sz,  ticket, protocols, objectType, false );
    if ( rc == 0 ) {
        SKVCheck c;
        SKVCheckInit ( & c, acc, version, protocols );
        VectorForEach
            ( & service . req . cgiReq . params, false, SCgiRequestCheck, & c );
        rc = c . passed;
    }
    {
        rc_t r2 = KServiceFini ( & service );
        if ( rc == 0 ) {
            rc = r2;
        }
    }
    return rc;
}

typedef struct {
    const char * id;
    EObjectType type;
    const char * ticket;
} SServiceRequestTestData;

rc_t KServiceNamesRequestTest ( const KNSManager * mgr, const char * b,
    const char * cgi, VRemoteProtocols protocols,
    const SServiceRequestTestData * d, ... )
{
    va_list args;
    KService * service = NULL;
    KStream * stream = NULL;
    rc_t rc = KServiceMakeWithMgr ( & service, mgr);
    va_start ( args, d );
    while ( rc == 0 && d != NULL ) {
        if ( d -> id != NULL ) {
            rc = KServiceAddObject ( service, d -> id, 0, d -> type );
        }
        if ( rc == 0 && d -> ticket != NULL ) {
            rc = KServiceAddTicket ( service, d -> ticket );
        }
        d = va_arg ( args, const SServiceRequestTestData * );
    }
    if ( rc == 0 ) {
        rc = KServiceInitNamesRequest ( service, protocols, cgi );
    }
    if ( rc == 0 ) {
        SKVCheck c;
    /*SKVCheckInit ( & c, acc, service -> req . version .raw . s, protocols );*/
        VectorForEach (
            & service -> req . cgiReq . params, false, SCgiRequestCheck, & c );
        rc = c . passed;
    }
    if ( rc == 0 ) {
        rc = KBufferStreamMake ( & stream, b, string_size ( b ) );
    }
    if ( rc == 0 ) {
        rc = KServiceProcessStream ( service, stream );
    }
    if ( rc == 0 ) {
        const KSrvResponse * l = NULL;
        rc = KServiceGetResponse ( service, & l );
        if ( rc == 0 ) {
            uint32_t i = 0;
            uint32_t n = KSrvResponseLength ( l );
            for ( i = 0; rc == 0 && i < n; ++i ) {
                const VPathSet * s = NULL;
                rc = KSrvResponseGet ( l, i, & s );
                RELEASE ( VPathSet, s );
            }
        }
        RELEASE ( KSrvResponse, l );
    }
    RELEASE ( KStream, stream );
    RELEASE ( KService, service );
    return rc;
}

rc_t KServiceFuserTest ( const KNSManager * mgr,  const char * ticket,
    const char * acc, ... )
{
    va_list args;
    KService * service = NULL;
    const KSrvResponse * response = NULL;
    rc_t rc = KServiceMake ( & service);
    va_start ( args, acc );
    while ( rc == 0 && acc != NULL ) {
        rc = KServiceAddId ( service, acc );
        acc = va_arg ( args, const char * );
    }
    if ( rc == 0 ) {
        rc = KServiceNamesExecute ( service, eProtocolDefault, & response );
    }
    if ( rc == 0 ) {
        uint32_t i = 0;
        for ( i = 0; rc == 0 && i < KSrvResponseLength ( response ); ++i ) {
            const VPath * path = NULL;
            rc = KSrvResponseGetPath ( response, i, 0, & path, NULL );
            if ( rc == 0 ) {
                rc_t r2;
/*KTime_t mod = VPathGetModDate ( path );size_t size = VPathGetSize ( path );*/
                String id;
                memset ( & id, 0, sizeof id );
                r2 = VPathGetId ( path, & id );
                if ( rc == 0 )
                    rc = r2;
            }
            RELEASE ( VPath, path );
        }
    }
    RELEASE ( KSrvResponse, response );
    RELEASE ( KService, service );
    return rc;
}

rc_t SCgiRequestPerformTestNames1 ( const KNSManager * mgr, const char * cgi,
    const char * version, const char * acc, const char * ticket,
    VRemoteProtocols protocols, EObjectType objectType )
{
    KService service;
    rc_t rc = KServiceInitNames1 ( & service, mgr, cgi, version, acc,
        string_measure ( acc, NULL ), ticket, protocols, objectType, false );
    if ( rc == 0 ) {
        KStream * response = NULL;
        rc = SCgiRequestPerform
            ( & service . req . cgiReq, & service . helper, & response );
        RELEASE ( KStream, response );
    }
    {
        rc_t r2 = KServiceFini ( & service );
        if ( rc == 0 ) {
            rc = r2;
        }
    }
    return rc;
}

rc_t KServiceProcessStreamTestNames1 ( const KNSManager * mgr,
    const char * b, const char * version, const char * acc,
    const VPath * exp, const char * ticket, const VPath * ex2,
    int errors )
{
    KService service;
    KStream * stream = NULL;
    rc_t rc = 0;
    if ( rc == 0 )
        rc = KServiceInitNames1 ( & service, mgr, "", version, acc,
            string_measure ( acc, NULL ), ticket, eProtocolHttp,
            eOT_undefined, false );
    if ( rc == 0 )
        rc = KBufferStreamMake ( & stream, b, string_size ( b ) );
    if ( rc == 0 ) {
        KServiceExpectErrors ( & service, errors );
    }
    if ( rc == 0 ) {
        rc = KServiceProcessStream ( & service, stream );
    }
    if ( rc == 0 ) {
        if ( VectorLength ( & service . resp . rows ) != 1) {
            rc = 1;
        }
        else {
            const VPath * path = NULL;
            const SRow * r
                = ( SRow * ) VectorGet ( & service . resp . rows, 0 );
            if ( r == NULL) {
                rc = 2;
            }
            else {
                path = r -> path . http;
            }
            if ( exp != NULL && rc == 0 ) {
                int notequal = ~ 0;
                rc = VPathEqual ( path, exp, & notequal );
                if ( rc == 0 ) {
                    rc = notequal;
                }
            }
            if ( ex2 != NULL && rc == 0 ) {
                int notequal = ~ 0;
                rc = VPathEqual ( path, ex2, & notequal );
                if ( rc == 0 ) {
                    rc = notequal;
                }
            }
        }
    }
    {
        rc_t r2 = KServiceFini ( & service );
        if ( rc == 0 ) {
            rc = r2;
        }
    }
    RELEASE ( KStream, stream );
    return rc;
}

rc_t KServiceNames3_0StreamTest ( const char * buffer,
    const KSrvResponse ** response )
{
    KService service;
    KStream * stream = NULL;
    rc_t rc = 0;
    rc_t r2 = 0;
    if ( rc == 0 )
        rc = KServiceInit ( & service, NULL );
    if ( rc == 0 )
        rc = KBufferStreamMake ( & stream, buffer, string_size ( buffer ) );
    if ( rc == 0 )
        rc = KServiceProcessStream ( & service, stream );
    if ( rc == 0 )
        rc = KServiceGetResponse ( & service , response );
    r2 = KServiceFini ( & service );
    if ( rc == 0 )
        rc = r2;
    RELEASE ( KStream, stream );
    return rc;
}

rc_t KServiceCgiTest1 ( const KNSManager * mgr, const char * cgi,
    const char * version, const char * acc, const char * ticket,
    VRemoteProtocols protocols, EObjectType objectType,
    const VPath * exp, const VPath * ex2 )
{
    const VPath * path = NULL;
    rc_t rc = KService1NameWithVersionAndType ( mgr, cgi, acc,
        string_measure ( acc, NULL ), ticket, protocols,
        & path, NULL, false, version, objectType );
    if ( rc == 0 ) {
        if ( exp != NULL && rc == 0 ) {
            int notequal = ~ 0;
            rc = VPathEqual ( path, exp, & notequal );
            if ( rc == 0 ) {
                rc = notequal;
            }
        }
        if ( ex2 != NULL && rc == 0 ) {
            int notequal = ~ 0;
            rc = VPathEqual ( path, ex2, & notequal );
            if ( rc == 0 ) {
                rc = notequal;
            }
        }
    }
    RELEASE ( VPath, path );
    return rc;
}

rc_t KServiceSearchTest1
    ( const KNSManager * mgr, const char * cgi, const char * acc )
{
    rc_t rc = 0;
    KService service;
    const Kart * result = NULL;
    rc = KServiceInit ( & service, mgr );
    if ( rc == 0 ) {
        rc = KServiceAddId ( & service, acc );
    }
    if ( rc == 0 ) {
        rc = KServiceSearchExecute ( & service, & result );
    }
    {
        rc_t r2 = KServiceFini ( & service );
        if ( rc == 0 ) {
            rc = r2;
        }
    }
    RELEASE ( Kart, result );
    return rc;
}

rc_t KServiceSearchTest (
    const KNSManager * mgr, const char * cgi, const char * acc, ... )
{
    va_list args;
    rc_t rc = 0;
    KStream * stream = NULL;
    const Kart * result = NULL;
    KService service;
    rc = KServiceInit ( & service, mgr );
    va_start ( args, acc );
    while ( rc == 0 && acc != NULL ) {
        rc = KServiceAddObject ( & service, acc, 0, eOT_undefined);
        acc = va_arg ( args, const char * );
    }
    if ( rc == 0 ) {
        rc = KServiceSearchExecuteExt ( & service, cgi, NULL, & result );
    }
    {
        rc_t r2 = KartRelease ( result );
        if ( rc == 0 ) {
            rc = r2;
        }
    }
    {
        rc_t r2 = KServiceFini ( & service );
        if ( rc == 0 ) {
            rc = r2;
        }
    }
    RELEASE ( KStream, stream );
    return rc;
}

/******************************************************************************/

/*   ResponseRow */

#if 0
rc_t ResponseRowGetType   ( const ResponseRow * self, const EObjectType * res );
rc_t ResponseRowGetId     ( const ResponseRow * self, const String      * res );
rc_t ResponseRowGetSize   ( const ResponseRow * self, const size_t      * res );
rc_t ResponseRowGetDate   ( const ResponseRow * self, const KTime       * res );
rc_t ResponseRowGetMd5    ( const ResponseRow * self, const uint8_t     * res );
rc_t ResponseRowGetTicket ( const ResponseRow * self, const String      * res );
rc_t ResponseRowGetUrl    ( const ResponseRow * self, const String      * res );
rc_t ResponseRowGetCode   ( const ResponseRow * self, const uint32_t    * res );
rc_t ResponseRowGetMessage( const ResponseRow * self, const String      * res );

/*   VPath */
rc_t VPathsInit ( const VPath * query );
rc_t VPathsLocal ( const VPaths * self, const VPath * result );
rc_t VPathsRemoteHttp ( const VPaths * self, const VPath * result );
rc_t VPathsRemoteFasp ( const VPaths * self, const VPath * result );
rc_t VPathsCache ( const VPaths * self, const VPath * result );
rc_t VPathsQuery ( VPaths * self );

rc_t Names3KStreamToResponseRowList ( const ServicesRequest * request,
     const KStream * stream, replyStrList ** list );
#endif
