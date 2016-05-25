#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#ifdef SZIP_FILTER
#include <szlib.h>
#endif
#ifdef BZIP2_FILTER
#include <bzlib.h>
#endif
#ifdef FPZIP_FILTER
#include <fpzip.h>
#endif
#ifdef ZFP_FILTER
#include <zfp.h>
#endif

#ifndef SZIP_FILTER
#define SZ_MAX_PIXELS_PER_BLOCK 0
#endif

#include "netcdf.h"
#include "hdf5.h"
#include "nc4compress.h"
#include "nc_logging.h"

#define DEBUG

#define VERIFYSIZE 1

/* From hdf5.H5private.h */
#define H5_ASSIGN_OVERFLOW(dst, src, srctype, dsttype)  \
    (dst) = (dsttype)(src);
#define H5_CHECK_OVERFLOW(var, vartype, casttype)

/* From hdf5.H5Fprivate.h */
#  define UINT32DECODE(p, i) {						      \
   (i)	=  (uint32_t)(*(p) & 0xff);	   (p)++;			      \
   (i) |= ((uint32_t)(*(p) & 0xff) <<  8); (p)++;			      \
   (i) |= ((uint32_t)(*(p) & 0xff) << 16); (p)++;			      \
   (i) |= ((uint32_t)(*(p) & 0xff) << 24); (p)++;			      \
}
#  define UINT32ENCODE(p, i) {						      \
   *(p) = (uint8_t)( (i)        & 0xff); (p)++;				      \
   *(p) = (uint8_t)(((i) >>  8) & 0xff); (p)++;				      \
   *(p) = (uint8_t)(((i) >> 16) & 0xff); (p)++;				      \
   *(p) = (uint8_t)(((i) >> 24) & 0xff); (p)++;				      \
}

typedef struct NCC_COMPRESSOR {
    NC_algorithm nccid;
    char name[NC_COMPRESSION_MAX_NAME+1]; /* canonical compressor name */
    size_t nelems; /* size of the compression parameters in units of uint32 */
    H5Z_filter_t h5filterid;
    unsigned int flags; /* currently unused */
    /* Tell HDF5 about the filter */
    int (*_register)(const struct NCC_COMPRESSOR*, H5Z_class2_t*);
    /* Attach a set of parameters to a specific variable (via plist)
       Must also do any necessary input parameter validations.
    */
    int (*_attach)(const struct NCC_COMPRESSOR*,NC_compression_info* info, hid_t vid, int rank, size_t* chunksizes);
} NCC_COMPRESSOR;

#define H5Z_FILTER_BZIP2 307
#define H5Z_FILTER_FPZIP 256
#define H5Z_FILTER_ZFP 257
#define H5Z_FILTER_JP2 258

static H5Z_class2_t H5Z_INFO[NC_NCOMPRESSORS];
static int registered[NC_NCOMPRESSORS];

static int zip_valid(NC_compression_info*,int,size_t*);
static int szip_valid(NC_compression_info*,int,size_t*);
static int bzip2_valid(NC_compression_info*,int,size_t*);
static int fpzip_valid(NC_compression_info*,int,size_t*);
static int zfp_valid(NC_compression_info*,int,size_t*);

/*Forward*/
#ifdef BZIP2_FILTER
static size_t H5Z_filter_bzip2(unsigned,size_t,const unsigned[],size_t,size_t*,void**);
#endif
#ifdef FPZIP_FILTER
static size_t H5Z_filter_fpzip(unsigned,size_t,const unsigned[],size_t,size_t*,void**);
static int fpzip_init(NC_compression_info* info, int rank, size_t* chunksizes);
#endif
#ifdef ZFP_FILTER
static size_t H5Z_filter_zfp(unsigned,size_t,const unsigned[],size_t,size_t*,void**);
static int zfp_init(NC_compression_info* info, int rank, size_t* chunksizes);

#if 0
struct ZFP_PARAMS {
    zfp_stream* stream;    /* compressed stream */
    unsigned int minbits;  /* minimum number of bits per 4^d block */
    unsigned int maxbits;  /* maximum number of bits per 4^d block */
    unsigned int maxprec;  /* maximum precision (# bit planes coded) */
    int minexp;            /* minimum base-2 exponent; error <= 2^minexp */
    bitstream* bstream;
    zfp_field* field;
    unsigned int dims;
    void* buffer;
    size_t bufsize;
};

#define ZP_UINT_SIZE ((sizeof(struct ZFP_PARAMS) + (sizeof(unsigned int) - 1))/sizeof(unsigned int))

union ZFP_ARGV {
    struct ZFP_PARAMS zparams;
    unsigned int argv[ZP_UINT_SIZE];
};
#endif

#endif /*ZFP_FILTER*/

#ifndef DEBUG
#define THROW(e) (e)
#else
#define THROW(e) checkerr(e,__FILE__,__LINE__)
static int checkerr(int e, const char* file, int line)
{
   if(e != 0) {
     fprintf(stderr, "Error %d in file %s, line %d.\n", e, file, line);
     fflush(stderr);
     abort();
   }
   return e;
}
#endif

/* Forward */
static int available(const NCC_COMPRESSOR* info, H5Z_class2_t*);
static const NCC_COMPRESSOR compressors[NC_NCOMPRESSORS+1];
static const NCC_COMPRESSOR* NC_compressor_for(NC_algorithm index);
static void verifysize();

/*
Turn on compression for a variable's plist
*/
int
NC_compress_set(NC_compression_info* info, hid_t vid, int rank, size_t* chunksizes)
{
    const NCC_COMPRESSOR* cmp = NC_compressor_for(info->algorithm);
    int stat = NC_NOERR;
    if(cmp == NULL)
        LOG((1,"%s: error: unsupported compression: %d",
	    __func__, info->algorithm));
    if(!registered[cmp->nccid])
        LOG((1,"%s: error: unregistered compression: %s",
	    __func__, NC_algorithm_name(info->algorithm)));
    stat = cmp->_attach(cmp,info,vid,rank,chunksizes);
    return THROW(stat);
}

/* 
Register all known filters with the library
*/
int
NC_compress_register_all(void)
{
    const NCC_COMPRESSOR* cmp;
#ifdef VERIFYSIZE
    verifysize();
#endif
    memset(H5Z_INFO,0,sizeof(H5Z_INFO));
    for(cmp=compressors;cmp->nccid;cmp++) {
        H5Z_class2_t* h5info = &H5Z_INFO[cmp->nccid];
	h5info->version = H5Z_CLASS_T_VERS;
	h5info->encoder_present = 1;
	h5info->decoder_present = 1;
	h5info->can_apply = NULL;
	h5info->set_local = NULL;
	h5info->name = cmp->name;
	h5info->id = cmp->h5filterid;
	if(cmp->_register != NULL) {
	    int stat = cmp->_register(cmp,h5info);
	    if(stat != NC_NOERR)
		return stat;
            if(available(cmp,h5info) != NC_NOERR)
                return THROW(NC_ECOMPRESS);
	}
    }
    return THROW(NC_NOERR);
}

#if 0
int
NC_compress_inq_argv(hid_t h5filterid, /*in*/
                     int findex, /*in*/
                     NC_algorithm* algp, /*out*/
                     int* nelemsp, /*out*/
                     unsigned int* elems /*out*/
                     )
{
    H5Z_filter_t filter;
    NC_algorithm alg = NC_NOZIP;
    size_t nelems = 0;
    const NCC_COMPRESSOR* cmp;
    int stat = NC_NOERR;
    if((filter = H5Pget_filter2(h5filterid, findex, NULL, &nelems, elems, 0, NULL, NULL)) < 0) 
	{stat = NC_EHDFERR; goto fail;}
    alg = NC_algorithm_for_filter(filter);
    if(alg == NC_NOZIP
       || filter == H5Z_FILTER_SHUFFLE
       || filter == H5Z_FILTER_FLETCHER32) {
	if(nelemsp) *nelemsp = 0;
	stat = NC_ECOMPRESS; goto fail;
    }
    cmp = NC_compressor_for(alg);
    if(algp) *algp = alg;
    if(nelemsp) *nelemsp = nelems;
    return THROW(NC_NOERR);
fail:
    return THROW(stat);		
}
#endif

/*
* Check if compression is available and can be used for both
* compression and decompression.  Normally we do not perform error
* checking in these examples for the sake of clarity, but in this
* case we will make an exception because this filter is an
* optional part of the hdf5 library.
*/
static int
available(const NCC_COMPRESSOR* info, H5Z_class2_t* h5info)
{
    htri_t avail;
    unsigned int filter_info;

    if(registered[info->nccid]) {
        avail = H5Zfilter_avail(h5info->id);
        if(!avail) {
            fprintf(stderr,"Filter not available: %s.\n",info->name);
            return THROW(NC_ECOMPRESS);
	}
        if(H5Zget_filter_info(h5info->id, &filter_info))
	    return THROW(NC_ECOMPRESS);
        if(!(filter_info & H5Z_FILTER_CONFIG_ENCODE_ENABLED)
           || !(filter_info & H5Z_FILTER_CONFIG_DECODE_ENABLED) ) {
           fprintf(stderr,"Filter not available for encoding and decoding: %s.\n",info->name);
            return THROW(NC_ECOMPRESS);
	}
    }
    return THROW(NC_NOERR);
}

#if 0
/**
Generic inquiry function
*/
static int
generic_inq(const NCC_COMPRESSOR* info,
	    hid_t plist,
	    int* argc,
	    NCC_PARAMS* params)
{
    int i;
    if(!registered[info->nccid])
	return THROW(NC_ECOMPRESS);
    if(argc == NULL || *argc != info->nelems)
       return THROW(NC_EINVAL);
    *argc = info->nelems;
    for(i=0;i<*argc;i++)
        params->params[i] = argv[i];
    return THROW(NC_NOERR);
}
#endif

/**************************************************/
/*#ifdef ZIP (DEFLATE) compression always defined */

static int
zip_register(const NCC_COMPRESSOR* info, H5Z_class2_t* h5info)
{
    registered[NC_ZIP] = 1;
    return THROW(NC_NOERR); /* no-op */
}

static int
zip_attach(const NCC_COMPRESSOR* cmp, NC_compression_info* info, hid_t vid, int rank, size_t* chunksizes)
{
    int stat = NC_NOERR;
    stat = zip_valid(info,rank,chunksizes);
    if(stat == NC_NOERR) {
        if(H5Pset_deflate(vid, info->params.zip.level))
	stat = NC_ECOMPRESS;
    }
    return THROW(stat);
}

static int
zip_valid(NC_compression_info* info, int rank, size_t* chunksizes)
{
    int stat = NC_NOERR;
    /* validate level */
    if(info->params.zip.level < NC_DEFLATE_LEVEL_MIN ||
	info->params.zip.level > NC_DEFLATE_LEVEL_MAX)
	stat =  NC_EINVAL;
    return THROW(stat);
}

/**************************************************/

static int
szip_register(const NCC_COMPRESSOR* cmp, H5Z_class2_t* h5info)
{
    herr_t stat = NC_NOERR;
#ifdef SZIP_FILTER
    /* See if already in the hdf5 library */
    int avail = H5Zfilter_avail(H5Z_FILTER_SZIP);
    if(avail) {
        registered[info->nccid] = (avail ? 1 : 0);
    }
#endif
    return THROW((stat ? NC_ECOMPRESS : NC_NOERR));
}

static int
szip_attach(const NCC_COMPRESSOR* cmp, NC_compression_info* info, hid_t vid, int rank, size_t* chunksizes)
{
    int stat = NC_NOERR;
    stat = szip_valid(info,rank,chunksizes);
    if(stat == NC_NOERR) {
        /* See if already in the hdf5 library */
        int avail = H5Zfilter_avail(H5Z_FILTER_SZIP);
        if(avail) {
	    if(H5Pset_szip(vid, info->params.szip.options_mask, info->params.szip.pixels_per_block))
	        stat = NC_ECOMPRESS;
	}
    }
    return THROW(stat);
}

static int
szip_valid(NC_compression_info* info, int rank, size_t* chunksizes)
{
    int stat = NC_NOERR;
    /* validate bpp */
    if(info->params.szip.pixels_per_block > SZ_MAX_PIXELS_PER_BLOCK)
	stat = NC_EINVAL;
    return THROW(stat);
}

#if 0
#ifdef SZIP_FILTER
static size_t
H5Z_filter_szip (unsigned flags, size_t cd_nelmts, const unsigned cd_values[],
   size_t nbytes, size_t *buf_size, void **buf)
{
    size_t ret_value = 0;       /* Return value */
    size_t size_out  = 0;       /* Size of output buffer */
    unsigned char *outbuf = NULL;    /* Pointer to new output buffer */
    unsigned char *newbuf = NULL;    /* Pointer to input buffer */
    SZ_com_t sz_param;          /* szip parameter block */

    /* Sanity check to make certain that we haven't drifted out of date with
     * the mask options from the szlib.h header */
    assert(H5_SZIP_ALLOW_K13_OPTION_MASK==SZ_ALLOW_K13_OPTION_MASK);
    assert(H5_SZIP_CHIP_OPTION_MASK==SZ_CHIP_OPTION_MASK);
    assert(H5_SZIP_EC_OPTION_MASK==SZ_EC_OPTION_MASK);
#if 0 /* not defined in our szlib.h */
    assert(H5_SZIP_LSB_OPTION_MASK==SZ_LSB_OPTION_MASK);
    assert(H5_SZIP_MSB_OPTION_MASK==SZ_MSB_OPTION_MASK);
    assert(H5_SZIP_RAW_OPTION_MASK==SZ_RAW_OPTION_MASK);
#endif
    assert(H5_SZIP_NN_OPTION_MASK==SZ_NN_OPTION_MASK);

    /* Check arguments */
    if (cd_nelmts!=4) {
	fprintf(stderr,"szip: invalid deflate aggression level\n");
	ret_value = 0;
	goto done;
    }

    /* Copy the filter parameters into the szip parameter block */
    H5_ASSIGN_OVERFLOW(sz_param.options_mask,cd_values[H5Z_SZIP_PARM_MASK],unsigned,int);
    H5_ASSIGN_OVERFLOW(sz_param.bits_per_pixel,cd_values[H5Z_SZIP_PARM_BPP],unsigned,int);
    H5_ASSIGN_OVERFLOW(sz_param.pixels_per_block,cd_values[H5Z_SZIP_PARM_PPB],unsigned,int);
    H5_ASSIGN_OVERFLOW(sz_param.pixels_per_scanline,cd_values[H5Z_SZIP_PARM_PPS],unsigned,int);

    /* Input; uncompress */
    if (flags & H5Z_FLAG_REVERSE) {
        uint32_t stored_nalloc;  /* Number of bytes the compressed block will expand into */
        size_t nalloc;  /* Number of bytes the compressed block will expand into */

        /* Get the size of the uncompressed buffer */
        newbuf = *buf;
        UINT32DECODE(newbuf,stored_nalloc);
        H5_ASSIGN_OVERFLOW(nalloc,stored_nalloc,uint32_t,size_t);

        /* Allocate space for the uncompressed buffer */
        if(NULL==(outbuf = malloc(nalloc))) {
	    fprintf(stderr,"szip: memory allocation failed for szip decompression\n");
	    ret_value = 0;
	    goto done;
	}
        /* Decompress the buffer */
        size_out=nalloc;
        if(SZ_BufftoBuffDecompress(outbuf, &size_out, newbuf, nbytes-4, &sz_param) != SZ_OK) {
	    fprintf(stderr,"szip: szip_filter: decompression failed\n");
	    ret_value = 0;
	    goto done;
	}
        assert(size_out==nalloc);

        /* Free the input buffer */
        if(*buf) free(*buf);

        /* Set return values */
        *buf = outbuf;
        outbuf = NULL;
        *buf_size = nalloc;
        ret_value = nalloc;
    }
    /* Output; compress */
    else {
        unsigned char *dst = NULL;    /* Temporary pointer to new output buffer */

        /* Allocate space for the compressed buffer & header (assume data won't get bigger) */
        if(NULL==(dst=outbuf = malloc(nbytes+4))) {
	    fprintf(stderr,"szip: unable to allocate szip destination buffer\n");
	    ret_value = 0;
	    goto done;
	}
        /* Encode the uncompressed length */
        H5_CHECK_OVERFLOW(nbytes,size_t,uint32_t);
        UINT32ENCODE(dst,nbytes);

        /* Compress the buffer */
        size_out = nbytes;
        if(SZ_OK!= SZ_BufftoBuffCompress(dst, &size_out, *buf, nbytes, &sz_param)) {
	    fprintf(stderr,"szip: overflow\n");
	    ret_value = 0;
	    goto done;
	}
        assert(size_out<=nbytes);

        /* Free the input buffer */
        if(*buf) free(*buf);

        /* Set return values */
        *buf = outbuf;
        outbuf = NULL;
        *buf_size = size_out+4;
        ret_value = size_out+4;
    }
done:
    if(outbuf)
        free(outbuf);
    return ret_value;
}
#endif /*SZIP_FILTER*/
#endif /*0*/

/**************************************************/

static int
bzip2_register(const NCC_COMPRESSOR* cmp, H5Z_class2_t* h5info)
{
    herr_t stat = NC_NOERR;
#ifdef BZIP2_FILTER
    /* finish the H5Z_class2_t instance */
    h5info->filter = (H5Z_func_t)H5Z_filter_bzip2;
    stat = H5Zregister(h5info);
    if(stat == 0) registered[cmp->nccid] = 1;
#endif
    return THROW((stat != 0 ? NC_ECOMPRESS : NC_NOERR));
};

static int
bzip2_attach(const NCC_COMPRESSOR* cmp, NC_compression_info* info, hid_t vid, int rank, size_t* chunksizes)
{
    int stat = NC_NOERR;
#ifdef BZIP2_FILTER
    stat = bzip2_valid(info,rank,chunksizes);
    if(stat == NC_NOERR) {
        if(H5Pset_filter(vid, cmp->h5filterid, H5Z_FLAG_MANDATORY, (size_t)NC_NELEMS_BZIP2,info->params.argv))
	    stat = NC_ECOMPRESS;
    }
#endif
    return THROW(stat);
}

static int
bzip2_valid(NC_compression_info* info, int rank, size_t* chunksizes)
{
    int stat = NC_NOERR;
    if(info->params.bzip2.level < NC_DEFLATE_LEVEL_MIN ||
       info->params.bzip2.level > NC_DEFLATE_LEVEL_MAX)
	stat = NC_EINVAL;
    return THROW(stat);
}

#ifdef BZIP2_FILTER
static size_t
H5Z_filter_bzip2(unsigned int flags, size_t cd_nelmts,
                     const unsigned int argv[], size_t nbytes,
                     size_t *buf_size, void **buf)
{
    char *outbuf = NULL;
    size_t outbuflen, outdatalen;
    int ret;
  
    if(nbytes == 0) return 0; /* sanity check */

    if(flags & H5Z_FLAG_REVERSE) {
  
	/** Decompress data.
         **
         ** This process is troublesome since the size of uncompressed data
         ** is unknown, so the low-level interface must be used.
         ** Data is decompressed to the output buffer (which is sized
         ** for the average case); if it gets full, its size is doubled
         ** and decompression continues.  This avoids repeatedly trying to
         ** decompress the whole block, which could be really inefficient.
         **/
  
	bz_stream stream;
	char *newbuf = NULL;
	size_t newbuflen;
  
        /* Prepare the output buffer. */
        outbuflen = nbytes * 3 + 1;/* average bzip2 compression ratio is 3:1 */
        outbuf = malloc(outbuflen);
	if(outbuf == NULL) {
	    fprintf(stderr,"memory allocation failed for bzip2 decompression\n");
	    goto cleanupAndFail;
	}
        /* Use standard malloc()/free() for internal memory handling. */
        stream.bzalloc = NULL;
        stream.bzfree = NULL;
        stream.opaque = NULL;

        /* Start decompression. */
        ret = BZ2_bzDecompressInit(&stream, 0, 0);
        if(ret != BZ_OK) {
            fprintf(stderr, "bzip2 decompression start failed with error %d\n", ret);
            goto cleanupAndFail;
	}

        /* Feed data to the decompression process and get decompressed data. */
        stream.next_out = outbuf;
        stream.avail_out = outbuflen;
        stream.next_in = *buf;
        stream.avail_in = nbytes;
        do {
	    ret = BZ2_bzDecompress(&stream);
            if(ret < 0) {
                fprintf(stderr, "BUG: bzip2 decompression failed with error %d\n", ret);
                goto cleanupAndFail;
            }
            if(ret != BZ_STREAM_END && stream.avail_out == 0) {
                /* Grow the output buffer. */
                newbuflen = outbuflen * 2;
                newbuf = realloc(outbuf, newbuflen);
                if(newbuf == NULL) {
                    fprintf(stderr, "memory allocation failed for bzip2 decompression\n");
                    goto cleanupAndFail;
                }
                stream.next_out = newbuf + outbuflen;  /* half the new buffer behind */
                stream.avail_out = outbuflen;  /* half the new buffer ahead */
                outbuf = newbuf;
                outbuflen = newbuflen;
            }
        } while (ret != BZ_STREAM_END);

        /* End compression. */
        outdatalen = stream.total_out_lo32;
        ret = BZ2_bzDecompressEnd(&stream);
        if(ret != BZ_OK) {
            fprintf(stderr, "bzip2 compression end failed with error %d\n", ret);
            goto cleanupAndFail;
        }
    } else {

	/** Compress data.
         **
         ** This is quite simple, since the size of compressed data in the worst
         ** case is known and it is not much bigger than the size of uncompressed
         ** data.  This allows us to use the simplified one-shot interface to
         ** compression.
         **/
   
	unsigned int odatalen;  /* maybe not the same size as outdatalen */
        int blockSize100k = 9;
   
        /* Get compression block size if present. */
	if(cd_nelmts > 0) {
            blockSize100k = argv[0];
	    if(blockSize100k < 1 || blockSize100k > 9) {
		fprintf(stderr, "invalid compression block size: %d\n", blockSize100k);
                goto cleanupAndFail;
	    }
        }
    
        /* Prepare the output buffer. */
        outbuflen = nbytes + nbytes / 100 + 600;  /* worst case (bzip2 docs) */
        outbuf = malloc(outbuflen);
        if(outbuf == NULL) {
	    fprintf(stderr, "memory allocation failed for bzip2 compression\n");
            goto cleanupAndFail;
        }
    
        /* Compress data. */
        odatalen = outbuflen;
        ret = BZ2_bzBuffToBuffCompress(outbuf, &odatalen, *buf, nbytes,
                                       blockSize100k, 0, 0);
        outdatalen = odatalen;
        if(ret != BZ_OK) {
	    fprintf(stderr, "bzip2 compression failed with error %d\n", ret);
            goto cleanupAndFail;
        }
    }

    /* Always replace the input buffer with the output buffer. */
    free(*buf);
    *buf = outbuf;
    *buf_size = outbuflen;
    return outdatalen;
    
cleanupAndFail:
    if(outbuf)
        free(outbuf);
    return 0;
}
#endif    
    
/**************************************************/
    
static int
fpzip_register(const NCC_COMPRESSOR* cmp, H5Z_class2_t* h5info)
{
    herr_t stat = 0;
#ifdef FPZIP_FILTER
    /* finish the H5Z_class2_t instance */
    h5info->filter = (H5Z_func_t)H5Z_filter_fpzip;
    stat = H5Zregister(h5info);
    registered[cmp->nccid] = (stat ? 0 : 1);
#endif
    return THROW((stat ? NC_ECOMPRESS : NC_NOERR));
}
    
static int
fpzip_attach(const NCC_COMPRESSOR* cmp, NC_compression_info* info, hid_t vid, int rank, size_t* chunksizes)
{
    int stat = NC_NOERR;
#ifdef FPZIP_FILTER
    stat = fpzip_valid(info,rank,chunksizes);
    if(stat == NC_NOERR) {
        /* Because the init is fairly complex, do it once */
        if(fpzip_init(info,rank,chunksizes) != NC_NOERR
           || H5Pset_filter(vid,cmp->h5filterid,H5Z_FLAG_MANDATORY,NC_NELEMS_ZFP,info->params.argv) != NC_NOERR)
  	    stat = NC_ECOMPRESS;
    }
#endif
    return THROW(stat); 
}
    
static int
fpzip_valid(NC_compression_info* info, int rank, size_t* chunksizes)
{
    int stat = NC_NOERR;
#ifdef FPZIP_FILTER
    if(info->params.fpzip.precision < 0 || info->params.fpzip.precision > 64) {stat = NC_EINVAL; goto done;}
    if(info->params.fpzip.precision > 32
       && info->params.fpzip.type != FPZIP_TYPE_DOUBLE) {stat = NC_EINVAL; goto done;}
done:
#endif
    return THROW(stat);
}

#ifdef FPZIP_FILTER

static int
fpzip_init(NC_compression_info* info, int rank, size_t* chunksizes)
{
    int stat = NC_NOERR;
    int choice,onesies,i,isdouble;
    size_t totalsize,elemsize;
    size_t nx,ny,nz,nf,nzsize;
    struct fpzip_params* fpz = &info->params.fpzip;
    struct fpzip_reserved* resv = (struct fpzip_reserved*)fpz->reserved;

    for(onesies=0,totalsize=1,i=0;i<rank;i++) {
	totalsize *= chunksizes[i];
	if(chunksizes[i] > 1) onesies++;
    }

    choice = (onesies == 3 && rank > 3 ? 1 : 0);

    if(choice) {
        nx = ny = nz = nf = 1;
        for(i=0;i<rank;i++) {
	    if(chunksizes[i] > 1) {
  	        if(nx == 1) nx = chunksizes[i];
	        else if(ny == 1) ny = chunksizes[i];
	        else if(nz == 1) nz = chunksizes[i];
	    }
	}
    } else { /*prefix*/
        /* Do some computations */
        nzsize = 0;
        if(rank > 2) {
            for(nzsize=1,i=2;i<rank;i++)
	        nzsize *= chunksizes[i];
	}
    }

    isdouble = (fpz->type == FPZIP_TYPE_DOUBLE);

    /* Element size (in bytes) */
    elemsize = (isdouble ? sizeof(double) : sizeof(float));

    /* precision */
    if(fpz->precision == 0)
        fpz->precision = CHAR_BIT * elemsize;

    if(choice) {
	resv->nx = nx;
	resv->ny = ny;
	resv->nz = nz;
	resv->nf = nf;
    } else {/*prefix*/
	resv->nx = chunksizes[0];
	resv->ny = (rank >= 2 ? chunksizes[1] : 1);
	resv->nz = (rank >= 3 ? nzsize : 1);
	resv->nf = 1;
    }
    resv->totalsize = totalsize;
    return THROW(stat);
}

/**
Assumptions:
1. Each incoming block represents 1 complete chunk
2. If "choose" is enabled, then only 3 chunks can have
   value different from 1 (one).
*/
static size_t
H5Z_filter_fpzip(unsigned int flags, size_t cd_nelmts,
                     const unsigned int argv[], size_t nbytes,
                     size_t *buf_size, void **buf)
{
    int i;
    NC_compression_info* info;
    struct fpzip_params* finfo;
    struct fpzip_reserved* resv;
    int rank;
    int isdouble;
    int prec;
    char *outbuf = NULL;
    size_t databytes;
    size_t elemsize;
    size_t totalsize;
    size_t outbuf_used = 0;
    FPZ* fpz = NULL;

    if(nbytes == 0) return 0; /* sanity check */

    info = (NC_compression_info*)argv;
    finfo = &info->params.fpzip;
    resv = (struct fpzip_reserved*)finfo->reserved;

    isdouble = (finfo->type == FPZIP_TYPE_DOUBLE);
    prec = finfo->precision;
    totalsize = resv->totalsize;

    /* Element size (in bytes) */
    elemsize = (isdouble ? sizeof(double) : sizeof(float));

    /* size of uncompressed data */
    databytes = totalsize * elemsize;

    if(flags & H5Z_FLAG_REVERSE) {
        /** Decompress data **/

	/* Tell fpzip where to get the compressed data */
        fpz = fpzip_read_from_buffer(*buf);
        if(fpzip_errno != fpzipSuccess)
	    goto cleanupAndFail;

	/* Fill fpz */
	fpz->type = finfo->type;
	fpz->prec = finfo->precision;
	fpz->nx = resv->nx;
	fpz->ny = resv->ny;
	fpz->nz = resv->nz;
	fpz->nf = resv->nf;

        /* Create the decompressed data buffer */
	outbuf = (char*)malloc(databytes);

        /* Decompress into the output data buffer */
        outbuf_used = fpzip_read(fpz,outbuf);

        if(fpzip_errno == fpzipSuccess && outbuf_used == 0)
            fpzip_errno = fpzipErrorReadStream;

        if(fpzip_errno != fpzipSuccess)
	    goto cleanupAndFail;

        fpzip_read_close(fpz);
        if(fpzip_errno != fpzipSuccess)
	    goto cleanupAndFail;

        /* Replace the buffer given to us with our decompressed data buffer */
        free(*buf);
        *buf = outbuf;
        *buf_size = databytes;
        outbuf = NULL;
        return outbuf_used; /* # valid bytes */

    } else {
  
        /** Compress data **/

        /* Create the compressed data buffer */
        /* This is overkill because compression is smaller than uncompressed */
        outbuf = (char*)malloc(databytes);

	fpz = fpzip_write_to_buffer(outbuf,databytes);
	if(fpzip_errno != fpzipSuccess)
  	    goto cleanupAndFail;

	/* Fill fpz */
	fpz->type = finfo->type;
	fpz->prec = finfo->precision;
	fpz->nx = resv->nx;
	fpz->ny = resv->ny;
	fpz->nz = resv->nz;
	fpz->nf = resv->nf;

	outbuf_used = fpzip_write(fpz,*buf);
	if(outbuf_used == 0 && fpzip_errno  == fpzipSuccess)
	    fpzip_errno = fpzipErrorWriteStream;
	if(fpzip_errno != fpzipSuccess)
	    goto cleanupAndFail;
	fpzip_write_close(fpz);
	if(fpzip_errno != fpzipSuccess)
	    goto cleanupAndFail;
	
	/* Replace the buffer given to us with our decompressed data buffer */
	free(*buf);
	*buf = outbuf;
	*buf_size = databytes;
        outbuf = NULL;
        return outbuf_used; /* # valid bytes */
    }

cleanupAndFail:
    if(outbuf)
        free(outbuf);
    if(fpzip_errno != fpzipSuccess) {
	fprintf(stderr,"fpzip error: %s\n",fpzip_errstr[fpzip_errno]);
        fflush(stderr);
    }
    return 0;
}
#endif    

/**************************************************/

static int
zfp_register(const NCC_COMPRESSOR* cmp, H5Z_class2_t* h5info)
{
    herr_t stat = 0;
#ifdef ZFP_FILTER
    /* finish the H5Z_class2_t instance */
    h5info->filter = (H5Z_func_t)H5Z_filter_zfp;
    stat = H5Zregister(h5info);
    registered[cmp->nccid] = (stat ? 0 : 1);
#endif
    return THROW((stat ? NC_ECOMPRESS : NC_NOERR));
}
    
static int
zfp_attach(const NCC_COMPRESSOR* cmp, NC_compression_info* info, hid_t vid, int rank, size_t* chunksizes)
{
    int stat = NC_NOERR;
#ifdef ZFP_FILTER
    stat = zfp_valid(info,rank,chunksizes);
    if(stat == NC_NOERR) {
        void* args;
        /* Because the init is fairly complex, do it once */
        if(zfp_init(info,rank,chunksizes) == NC_NOERR
           || H5Pset_filter(vid,cmp->h5filterid,H5Z_FLAG_MANDATORY,NC_NELEMS_ZFP,info->params.argv) != NC_NOERR)
	    stat = NC_ECOMPRESS;
    }
#endif
    return THROW(stat);
}
    
static int
zfp_valid(NC_compression_info* info, int rank, size_t* chunksizes)
{
    int stat = NC_NOERR;
#ifdef ZFP_FILTER
    if(rank < 0 || rank > NC_COMPRESSION_MAX_DIMS) {stat = NC_EINVAL; goto done;}
    if(info->params.zfp.precision < 0 || info->params.zfp.precision > 64) {stat = NC_EINVAL; goto done;}
    if(info->params.zfp.precision > 32 && info->params.zfp.type != zfp_type_double) {stat = NC_EINVAL; goto done;}
done:
#endif
    return THROW(stat);
}

#ifdef ZFP_FILTER

static int
zfp_init(NC_compression_info* info, int rank, size_t* chunksizes)
{
    int stat = NC_NOERR;
    int choice,onesies,i;
    size_t totalsize,elemsize;
    size_t nx,ny,nz,nzsize;
    zfp_field* field = NULL;
    zfp_type type;
    zfp_stream* zstream = NULL;
    struct zfp_params* zinfo = NULL;
    struct zfp_reserved* zres = NULL;
    
    zinfo = &info->params.zfp;
    zres = (struct zfp_reserved*)zinfo->reserved;

    type = (zfp_type)zinfo->type;

    /* Compute total size of the chunk and if # of chunks with size > 1 */
    for(onesies=0,totalsize=1,i=0;i<rank;i++) {
	totalsize *= chunksizes[i];
	if(chunksizes[i] > 1) onesies++;
    }

    /* If we have exactly 3 dims of size 1, and more than 3 dimensions,
       then merge down to 3 dimensions
    */
    choice = (onesies == 3 && rank > 3);
    if(choice) {
        nx = ny = nz = 1;
        for(i=0;i<rank;i++) {
            if(chunksizes[i] > 1) {
                if(nx == 1) nx = chunksizes[i];
                else if(ny == 1) ny = chunksizes[i];
                else if(nz == 1) nz = chunksizes[i];
            }
        }
    } else { /*prefix*/
        /* Do some computations */
        nzsize = 0;
        if(rank > 2) {
            for(nzsize=1,i=2;i<rank;i++) {
                nzsize *= chunksizes[i];
            }
        }
    }

    if(!choice) {/*prefix*/
	nx = chunksizes[0];
	ny = (rank >= 2 ? chunksizes[1] : 1);
	nz = (rank >= 3 ? nzsize : 1);
    }
    zres->nx = nx;
    zres->ny = ny;
    zres->nz = nz;
    
    /* Set additional info */

    /* Setup the concise parameters */
    zstream = zfp_stream_open(NULL);
    if(zstream == NULL)	{stat = NC_ENOMEM; goto done;}
    zfp_stream_set_accuracy(zstream, zinfo->tolerance, type);
    zfp_stream_set_precision(zstream, zinfo->precision, type);
    zfp_stream_set_rate(zstream, zinfo->rate, type, rank, 0);
    zres->zfp_params = zfp_stream_mode(zstream);

    zres->totalsize = totalsize;
    zres->rank = rank;

done:
    if(zstream != NULL) zfp_stream_close(zstream);
    return THROW(stat);
}

/**
Assumptions:
1. Each incoming block represents 1 complete chunk
*/
static size_t
H5Z_filter_zfp(unsigned int flags, size_t cd_nelmts,
                     const unsigned int argv[], size_t nbytes,
                     size_t *buf_size, void **buf)
{
    int stat = NC_NOERR;
    NC_compression_info* info;
    zfp_field* zfp;
    char *outbuf = NULL;
    size_t databytes;
    size_t elemsize;
    size_t totalsize;
    size_t outbuf_used = 0;
    zfp_stream* zstream = NULL;
    bitstream* bstream = NULL;
    zfp_field* field = NULL;
    int isdouble;
    zfp_type type;
    struct zfp_params* zinfo = NULL;
    struct zfp_reserved* zres = NULL;
    
    if(nbytes == 0) return 0; /* sanity check */

    info = (NC_compression_info*)argv;
    zinfo = &info->params.zfp;
    zres = (struct zfp_reserved*)zinfo->reserved;

    type = (zfp_type)zinfo->type;
    isdouble = (type == FPZIP_TYPE_DOUBLE);
    totalsize = zres->totalsize;

    /* Element size (in bytes) */
    elemsize = (isdouble ? sizeof(double) : sizeof(float));

    /* size of uncompressed data */
    databytes = totalsize * elemsize;

    /* Build a field */
    switch (zres->rank) {
    case 1: field = zfp_field_1d(NULL,type,zres->nx); break;
    case 2: field = zfp_field_2d(NULL,type,zres->nx,zres->ny); break;
    default: field = zfp_field_3d(NULL,type,zres->nx,zres->ny,zres->nx); break;
    }
    if(field == NULL) {stat = NC_ENOMEM; goto done;}

    /* always use stride 1 */
    field->sx = field->sy = field->sz = 0; 

    /* Build a bit stream and zfp stream*/
    bstream = stream_open(*buf,*buf_size);
    if(bstream == NULL)	{stat = NC_ENOMEM; goto done;}
    zstream = zfp_stream_open(bstream);
    if(zstream == NULL)	{stat = NC_ENOMEM; goto done;}

    /* Set stream mode */    
    if(!zfp_stream_set_mode(zstream,zres->zfp_params))
	{stat = NC_ECOMPRESS; goto done;}

    if(flags & H5Z_FLAG_REVERSE) {
        /** Decompress data **/

        /* Create the decompressed data buffer and tell zfp */
        outbuf = (char*)malloc(databytes);
        zfp_field_set_pointer(field, outbuf);
	outbuf_used = databytes; /* assume all is used */

        /* Build a bit stream and zfp stream*/
        bstream = stream_open(*buf,*buf_size);
        if(bstream == NULL) {stat = NC_ENOMEM; goto done;}
        zstream = zfp_stream_open(bstream);
        if(zstream == NULL) {stat = NC_ENOMEM; goto done;}

        /* Decompress into the stream */
	zfp_stream_rewind(zstream);
	if(zfp_decompress(zstream,field) != 0)
	    {stat = NC_ECOMPRESS; goto done;}

        /* Replace the buffer given to us with our decompressed data buffer */
        free(*buf);
        *buf = outbuf;
        *buf_size = databytes;
	outbuf = NULL;
        return outbuf_used; /* # valid bytes */

    } else {
  
        /** Compress data **/

	/* Tell zfp about decompressed data */
	zfp_field_set_pointer(field, *buf);

        /* Create the compressed data buffer and tell zfp */

	/* Recompute output size */
	databytes = zfp_stream_maximum_size(zstream, field);
        if(!databytes) {stat = NC_ECOMPRESS; goto done;}
        outbuf = (char*)malloc(databytes);
	if(outbuf == NULL) {stat = NC_ENOMEM; goto done;}

	/* Cross the streams :-) */
        bstream = stream_open(outbuf, databytes);
        if(bstream == NULL) {stat = NC_ENOMEM; goto done;}
        zstream = zfp_stream_open(bstream);
        if(zstream == NULL) {stat = NC_ENOMEM; goto done;}

        /* Decompress into the stream */
	zfp_stream_rewind(zstream);
	outbuf_used = zfp_compress(zstream,field);
	if(outbuf_used == 0)
	    {stat = NC_ECOMPRESS; goto done;}

        /* Replace the buffer given to us with our compressed data buffer */
        free(*buf);
        *buf = outbuf;
        *buf_size = databytes;
	outbuf = NULL;
        return outbuf_used; /* # valid bytes */
    }

done:
    if(field != NULL) zfp_field_free(field);
    if(zstream != NULL) zfp_stream_close(zstream);
    if(bstream != NULL) stream_close(bstream);
    return THROW(stat);
}

#endif    

/**************************************************/
/* Utilities */

const char*
NC_algorithm_name(NC_algorithm id)
{
    const NCC_COMPRESSOR* p;
    for(p=compressors;p->nccid != NC_NOZIP;p++) {
	if(p->nccid == id) return p->name;
    }
    return NULL;    
}

NC_algorithm
NC_algorithm_id(const char* name)
{
    const NCC_COMPRESSOR* p;
    for(p=compressors;p->nccid != NC_NOZIP;p++) {
	if(strcmp(name,p->name)==0) return p->nccid;
    }
    return NC_NOZIP;    
}

/* get compressor info by enum */
static const NCC_COMPRESSOR*
NC_compressor_for(NC_algorithm index)
{
    const NCC_COMPRESSOR* p;
    for(p=compressors;p->nccid != NC_NOZIP;p++) {
	if(p->nccid == index) return p;
    }
    return NULL;
}

NC_algorithm
NC_algorithm_for_filter(H5Z_filter_t h5filterid)
{
    const NCC_COMPRESSOR* p;
    for(p=compressors;p->nccid != NC_NOZIP;p++) {
	if(p->h5filterid == h5filterid) return p->nccid;
    }
    return NC_NOZIP;
}

/* Convert NC_compression_info -> NC_compression_t */
int
NC_compress_cvt_from(NC_compression_info* src, size_t dstsize, unsigned int* dst0)
{
    int stat = NC_NOERR;
    nc_compression_t* dst = (nc_compression_t*)dst0;
    switch (src->algorithm) {
    case NC_ZIP:
	if(sizeof(dst->zip) > dstsize) return NC_ECOMPRESS;
        dst->zip = src->params.zip;
	break;
    case NC_BZIP2:
	if(sizeof(dst->bzip2) > dstsize) return NC_ECOMPRESS;
        dst->bzip2 = src->params.bzip2;
	break;
    case NC_SZIP:
	if(sizeof(dst->szip) > dstsize) return NC_ECOMPRESS;
        dst->szip = src->params.szip;
	break;
    case NC_FPZIP:
	if(sizeof(dst->fpzip) > dstsize) return NC_ECOMPRESS;
	dst->fpzip = src->params.fpzip;
	break;
    case NC_ZFP:
	if(sizeof(dst->zfp) > dstsize) return NC_ECOMPRESS;
	dst->zfp = src->params.zfp;
	break;
    default:
	stat = NC_ECOMPRESS;
    }
    return THROW(stat);
}

/* Convert nc_compression_t -> NC_compression_info */
int
NC_compress_cvt_to(NC_algorithm alg, size_t srcsize, unsigned int* src0, NC_compression_info* dst)
{
    int stat = NC_NOERR;
    nc_compression_t* src = (nc_compression_t*)src0;

    dst->algorithm = alg;
    switch (alg) {
    case NC_ZIP:
        if(sizeof(src->zip) < srcsize) return NC_ECOMPRESS;
	dst->argc = NC_NELEMS_ZIP;	
        dst->params.zip = src->zip;
	break;
    case NC_BZIP2:
        if(sizeof(src->bzip2) < srcsize) return NC_ECOMPRESS;
	dst->argc = NC_NELEMS_BZIP2;
        dst->params.bzip2 = src->bzip2;
	break;
    case NC_SZIP:
        if(sizeof(src->szip) < srcsize) return NC_ECOMPRESS;
	dst->argc = NC_NELEMS_SZIP;
        dst->params.szip = src->szip;
	break;
    case NC_FPZIP:
        if(sizeof(src->fpzip) < srcsize) return NC_ECOMPRESS;
	dst->argc = NC_NELEMS_FPZIP;
	dst->params.fpzip = src->fpzip;
	break;
    case NC_ZFP:
        if(sizeof(src->zfp) < srcsize) return NC_ECOMPRESS;
	dst->argc = NC_NELEMS_ZFP;
	dst->params.zfp = src->zfp;
	break;
    default:
	stat = NC_ECOMPRESS;
    }
    return THROW(stat);
}

size_t
NC_algorithm_nelems(NC_algorithm alg)
{
    const NCC_COMPRESSOR* cmp;
    cmp = NC_compressor_for(alg);
    if(cmp == NULL)
	return 0;
    return cmp->nelems;
}


/**************************************************/

size_t
nc_inq_algorithm_argc(const char* algname)
{
    NC_algorithm alg = NC_algorithm_id(algname);
    if(alg == NC_NOZIP) return 0;
    return NC_algorithm_nelems(alg);
}

static const char* algorithm_names[NC_NCOMPRESSORS+1] = {NULL};

/* Get set of known algorithms by name */
const char**
nc_inq_compressor_names(void)
{
    if(algorithm_names[0] == NULL) {
	int i,j;
	char** names = (char**)algorithm_names; /* break const */
	for(j=0,i=NC_NOZIP+1;i<NC_NCOMPRESSORS;i++) {
	    const NCC_COMPRESSOR* cmp = &compressors[i];
	    if(registered[i])
		names[j++] = (char*)cmp->name; /* remove const */
	}
	names[j] = NULL; /* NULL terminate */
    }
    return algorithm_names;
}

/**************************************************/
#ifdef VERIFYSIZE
static void
verifysize()
{
    int i;
    NC_compression_info info;
    for(i=0;i<NC_NCOMPRESSORS;i++) {
	int defined, computed, usize;
	switch (i) {
        case NC_ZIP:
	    computed = sizeof(info.params.zip);
	    defined = NC_NELEMS_ZIP;
	    break;
        case NC_SZIP:
	    computed = sizeof(info.params.szip);
	    defined = NC_NELEMS_SZIP;
	    break;
        case NC_BZIP2:
	    computed = sizeof(info.params.bzip2);
	    defined = NC_NELEMS_BZIP2;
	    break;
        case NC_FPZIP:
	    computed = sizeof(info.params.fpzip);
	    defined = NC_NELEMS_FPZIP;
	    break;
        case NC_ZFP:
	    computed = sizeof(info.params.zfp);
	    defined = NC_NELEMS_ZFP;
	    break;
	default:
	    computed = 0;
	    defined = 0;
	    break; /* ignore */
	}
	usize = computed / sizeof(unsigned int); /*fixup*/
        if((usize * sizeof(unsigned int)) != computed)
	    fprintf(stderr,"%s: partial size; computed=%d usize=%d\n",
			NC_algorithm_name(i),computed,usize);
	if(usize != defined) {
	    fprintf(stderr,"%s: size mismatch; computed=%d defined=%d\n",
			NC_algorithm_name(i),usize,defined);
	}
    }
}
#endif

/**************************************************/

/* Provide access to all the compressors */
static const NCC_COMPRESSOR compressors[NC_NCOMPRESSORS+1] = {
    {NC_ZIP, "zip", NC_NELEMS_ZIP, H5Z_FILTER_DEFLATE, 0, zip_register, zip_attach},
    {NC_SZIP, "szip", NC_NELEMS_SZIP, H5Z_FILTER_SZIP, 0, szip_register, szip_attach},
    {NC_BZIP2, "bzip2", NC_NELEMS_BZIP2, H5Z_FILTER_BZIP2, 0, bzip2_register, bzip2_attach},
    {NC_FPZIP, "fpzip", NC_NELEMS_FPZIP, H5Z_FILTER_FPZIP, 0, fpzip_register, fpzip_attach},
    {NC_ZFP, "zfp", NC_NELEMS_ZFP, H5Z_FILTER_ZFP, 0, zfp_register, zfp_attach},
    {NC_NOZIP, "", 0, 0, 0, NULL, NULL} /* must be last */
};