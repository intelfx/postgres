/*-------------------------------------------------------------------------
 *
 * toast_compression.c
 *	  Functions for toast compression.
 *
 * Copyright (c) 2021-2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/toast_compression.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef USE_LZ4
#include <lz4.h>
#endif
#ifdef USE_ZSTD
#include <zstd.h>
#endif

#include "access/detoast.h"
#include "access/toast_compression.h"
#include "common/pg_lzcompress.h"
#include "utils/memutils.h"
#include "varatt.h"

/* GUC */
int			default_toast_compression = TOAST_PGLZ_COMPRESSION;

#ifdef USE_ZSTD
/*
 * Compression and decompression contexts for ZSTD, preallocated for
 * performance
 */
static ZSTD_CCtx *zstd_cctx;
static ZSTD_DCtx *zstd_dctx;
#endif

#define NO_METHOD_SUPPORT(method) \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("compression method %s not supported", method), \
			 errdetail("This functionality requires the server to be built with %s support.", method)))

/*
 * Compress a varlena using PGLZ.
 *
 * Returns the compressed varlena, or NULL if compression fails.
 */
struct varlena *
pglz_compress_datum(const struct varlena *value)
{
	int32		valsize,
				len;
	struct varlena *tmp = NULL;

	valsize = VARSIZE_ANY_EXHDR(value);

	/*
	 * No point in wasting a palloc cycle if value size is outside the allowed
	 * range for compression.
	 */
	if (valsize < PGLZ_strategy_default->min_input_size ||
		valsize > PGLZ_strategy_default->max_input_size)
		return NULL;

	/*
	 * Figure out the maximum possible size of the pglz output, add the bytes
	 * that will be needed for varlena overhead, and allocate that amount.
	 */
	tmp = (struct varlena *) palloc(PGLZ_MAX_OUTPUT(valsize) +
									VARHDRSZ_COMPRESSED);

	len = pglz_compress(VARDATA_ANY(value),
						valsize,
						(char *) tmp + VARHDRSZ_COMPRESSED,
						NULL);
	if (len < 0)
	{
		pfree(tmp);
		return NULL;
	}

	SET_VARSIZE_COMPRESSED(tmp, len + VARHDRSZ_COMPRESSED);

	return tmp;
}

/*
 * Decompress a varlena that was compressed using PGLZ.
 */
struct varlena *
pglz_decompress_datum(const struct varlena *value)
{
	struct varlena *result;
	int32		rawsize;

	/* allocate memory for the uncompressed data */
	result = (struct varlena *) palloc(VARDATA_COMPRESSED_GET_EXTSIZE(value) + VARHDRSZ);

	/* decompress the data */
	rawsize = pglz_decompress((char *) value + VARHDRSZ_COMPRESSED,
							  VARSIZE(value) - VARHDRSZ_COMPRESSED,
							  VARDATA(result),
							  VARDATA_COMPRESSED_GET_EXTSIZE(value), true);
	if (rawsize < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed pglz data is corrupt")));

	SET_VARSIZE(result, rawsize + VARHDRSZ);

	return result;
}

/*
 * Decompress part of a varlena that was compressed using PGLZ.
 */
struct varlena *
pglz_decompress_datum_slice(const struct varlena *value,
							int32 slicelength)
{
	struct varlena *result;
	int32		rawsize;

	/* allocate memory for the uncompressed data */
	result = (struct varlena *) palloc(slicelength + VARHDRSZ);

	/* decompress the data */
	rawsize = pglz_decompress((char *) value + VARHDRSZ_COMPRESSED,
							  VARSIZE(value) - VARHDRSZ_COMPRESSED,
							  VARDATA(result),
							  slicelength, false);
	if (rawsize < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed pglz data is corrupt")));

	SET_VARSIZE(result, rawsize + VARHDRSZ);

	return result;
}

/*
 * Compress a varlena using LZ4.
 *
 * Returns the compressed varlena, or NULL if compression fails.
 */
struct varlena *
lz4_compress_datum(const struct varlena *value)
{
#ifndef USE_LZ4
	NO_METHOD_SUPPORT("lz4");
	return NULL;				/* keep compiler quiet */
#else
	int32		valsize;
	int32		len;
	int32		max_size;
	struct varlena *tmp = NULL;

	valsize = VARSIZE_ANY_EXHDR(value);

	/*
	 * Figure out the maximum possible size of the LZ4 output, add the bytes
	 * that will be needed for varlena overhead, and allocate that amount.
	 */
	max_size = LZ4_compressBound(valsize);
	tmp = (struct varlena *) palloc(max_size + VARHDRSZ_COMPRESSED);

	len = LZ4_compress_default(VARDATA_ANY(value),
							   (char *) tmp + VARHDRSZ_COMPRESSED,
							   valsize, max_size);
	if (len <= 0)
		elog(ERROR, "lz4 compression failed");

	/* data is incompressible so just free the memory and return NULL */
	if (len > valsize)
	{
		pfree(tmp);
		return NULL;
	}

	SET_VARSIZE_COMPRESSED(tmp, len + VARHDRSZ_COMPRESSED);

	return tmp;
#endif
}

/*
 * Decompress a varlena that was compressed using LZ4.
 */
struct varlena *
lz4_decompress_datum(const struct varlena *value)
{
#ifndef USE_LZ4
	NO_METHOD_SUPPORT("lz4");
	return NULL;				/* keep compiler quiet */
#else
	int32		rawsize;
	struct varlena *result;

	/* allocate memory for the uncompressed data */
	result = (struct varlena *) palloc(VARDATA_COMPRESSED_GET_EXTSIZE(value) + VARHDRSZ);

	/* decompress the data */
	rawsize = LZ4_decompress_safe((char *) value + VARHDRSZ_COMPRESSED,
								  VARDATA(result),
								  VARSIZE(value) - VARHDRSZ_COMPRESSED,
								  VARDATA_COMPRESSED_GET_EXTSIZE(value));
	if (rawsize < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed lz4 data is corrupt")));


	SET_VARSIZE(result, rawsize + VARHDRSZ);

	return result;
#endif
}

/*
 * Decompress part of a varlena that was compressed using LZ4.
 */
struct varlena *
lz4_decompress_datum_slice(const struct varlena *value, int32 slicelength)
{
#ifndef USE_LZ4
	NO_METHOD_SUPPORT("lz4");
	return NULL;				/* keep compiler quiet */
#else
	int32		rawsize;
	struct varlena *result;

	/* slice decompression not supported prior to 1.8.3 */
	if (LZ4_versionNumber() < 10803)
		return lz4_decompress_datum(value);

	/* allocate memory for the uncompressed data */
	result = (struct varlena *) palloc(slicelength + VARHDRSZ);

	/* decompress the data */
	rawsize = LZ4_decompress_safe_partial((char *) value + VARHDRSZ_COMPRESSED,
										  VARDATA(result),
										  VARSIZE(value) - VARHDRSZ_COMPRESSED,
										  slicelength,
										  slicelength);
	if (rawsize < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed lz4 data is corrupt")));

	SET_VARSIZE(result, rawsize + VARHDRSZ);

	return result;
#endif
}

/*
 * Compress a varlena using Zstandard.
 *
 * Returns the compressed varlena, or NULL if compression fails.
 */
struct varlena *
zstd_compress_datum(const struct varlena *value)
{
#ifndef USE_ZSTD
	NO_METHOD_SUPPORT("zstd");
	return NULL;				/* keep compiler quiet */
#else
	int32		valsize;
	size_t		len;
	size_t		max_size;
	struct varlena *tmp = NULL;

	valsize = VARSIZE_ANY_EXHDR(value);

	if (unlikely(zstd_cctx == NULL)) {
		zstd_cctx = ZSTD_createCCtx();

		if (unlikely(zstd_cctx == NULL))
			ereport(ERROR,
			        (errcode(ERRCODE_OUT_OF_MEMORY),
			         errmsg("out of memory"),
			         errdetail("Failed to allocate ZSTD context")));
	}

	/*
	 * Figure out the maximum possible size of the ZSTD output, add the bytes
	 * that will be needed for varlena overhead, and allocate that amount.
	 *
	 * For inputs close to the varlena size limit the worst case exceeds what
	 * palloc() accepts.  Report those as incompressible instead of failing
	 * the insert; the caller then stores the value uncompressed.
	 */
	max_size = ZSTD_compressBound(valsize);
	if (max_size > MaxAllocSize - VARHDRSZ_COMPRESSED)
		return NULL;

	tmp = (struct varlena *) palloc(max_size + VARHDRSZ_COMPRESSED);

	len = ZSTD_compressCCtx(zstd_cctx, (char *) tmp + VARHDRSZ_COMPRESSED,
	                        max_size, VARDATA_ANY(value), valsize,
	                        ZSTD_CLEVEL_DEFAULT);
	if (ZSTD_isError(len))
		elog(ERROR, "zstd compression failed: %s",
			 ZSTD_getErrorName(len));

	/* data is incompressible so just free the memory and return NULL */
	if (len > (size_t) valsize)
	{
		pfree(tmp);
		return NULL;
	}

	SET_VARSIZE_COMPRESSED(tmp, len + VARHDRSZ_COMPRESSED);

	return tmp;
#endif
}

/*
 * Decompress a varlena that was compressed using Zstandard.
 */
struct varlena *
zstd_decompress_datum(const struct varlena *value)
{
#ifndef USE_ZSTD
	NO_METHOD_SUPPORT("zstd");
	return NULL;				/* keep compiler quiet */
#else
	int32		rawsize;
	size_t		decompsize;
	struct varlena *result;

	rawsize = VARDATA_COMPRESSED_GET_EXTSIZE(value);

	/* allocate memory for the uncompressed data */
	result = (struct varlena *) palloc(rawsize + VARHDRSZ);

	/* decompress the data */
	decompsize = ZSTD_decompress(VARDATA(result),
								 rawsize,
								 (char *) value + VARHDRSZ_COMPRESSED,
								 VARSIZE(value) - VARHDRSZ_COMPRESSED);
	if (ZSTD_isError(decompsize))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed zstd data is corrupt: %s",
								 ZSTD_getErrorName(decompsize))));

	/*
	 * A frame that decodes to fewer bytes than the varlena header promises is
	 * corrupt; returning the short result would silently truncate the value.
	 */
	if (decompsize != (size_t) rawsize)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed zstd data is corrupt: expected %d bytes, got %zu",
								 rawsize, decompsize)));

	SET_VARSIZE(result, rawsize + VARHDRSZ);

	return result;
#endif
}

/*
 * Decompress part of a varlena that was compressed using Zstandard.
 *
 * ZSTD_decompress() is not able to decompress a partial portion, but streams
 * can do that.
 */
struct varlena *
zstd_decompress_datum_slice(const struct varlena *value, int32 slicelength)
{
#ifndef USE_ZSTD
	NO_METHOD_SUPPORT("zstd");
	return NULL;				/* keep compiler quiet */
#else

	struct varlena *result;

	ZSTD_inBuffer inBuf;
	ZSTD_outBuffer outBuf;
	size_t		ret;

	if (unlikely(zstd_dctx == NULL)) {
		zstd_dctx = ZSTD_createDCtx();

		if (unlikely(zstd_dctx == NULL))
			ereport(ERROR,
			        (errcode(ERRCODE_OUT_OF_MEMORY),
			         errmsg("out of memory"),
			         errdetail("Failed to allocate ZSTD context")));
	}

	/*
	 * Begin a new stream.  The context is shared, and a slice request that
	 * stopped before the end of the frame leaves it mid-stream, as does an
	 * error thrown out of the loop below.
	 */
	ret = ZSTD_initDStream(zstd_dctx);
	if (ZSTD_isError(ret))
		elog(ERROR, "could not reset zstd decompression context: %s",
			 ZSTD_getErrorName(ret));

	inBuf.src = (char *) value + VARHDRSZ_COMPRESSED;
	inBuf.size = VARSIZE(value) - VARHDRSZ_COMPRESSED;
	inBuf.pos = 0;

	result = (struct varlena *) palloc(slicelength + VARHDRSZ);

	outBuf.dst = (char *) result + VARHDRSZ;
	outBuf.size = slicelength;
	outBuf.pos = 0;

	while (inBuf.pos < inBuf.size &&
		   outBuf.pos < outBuf.size)
	{
		ret = ZSTD_decompressStream(zstd_dctx, &outBuf, &inBuf);

		if (ZSTD_isError(ret))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("compressed zstd data is corrupt: %s",
									 ZSTD_getErrorName(ret))));

		/* end of frame reached before the requested prefix was complete */
		if (ret == 0)
			break;
	}

	/*
	 * Callers never ask for more than the datum holds, so a short frame means
	 * the stored data is corrupt.
	 */
	if (outBuf.pos < outBuf.size)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed zstd data is corrupt: expected at least %d bytes, got %zu",
								 slicelength, outBuf.pos)));

	SET_VARSIZE(result, outBuf.pos + VARHDRSZ);

	return result;
#endif
}

/*
 * Extract compression ID from a varlena.
 *
 * Returns TOAST_INVALID_COMPRESSION_ID if the varlena is not compressed.
 */
ToastCompressionId
toast_get_compression_id(struct varlena *attr)
{
	ToastCompressionId cmid = TOAST_INVALID_COMPRESSION_ID;

	/*
	 * If it is stored externally then fetch the compression method id from
	 * the external toast pointer.  If compressed inline, fetch it from the
	 * toast compression header.
	 */
	if (VARATT_IS_EXTERNAL_ONDISK(attr))
	{
		struct varatt_external toast_pointer;

		VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

		if (VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer))
			cmid = VARATT_EXTERNAL_GET_COMPRESS_METHOD(toast_pointer);
	}
	else if (VARATT_IS_COMPRESSED(attr))
		cmid = VARDATA_COMPRESSED_GET_COMPRESS_METHOD(attr);

	return cmid;
}

/*
 * CompressionNameToMethod - Get compression method from compression name
 *
 * Search in the available built-in methods.  If the compression not found
 * in the built-in methods then return InvalidCompressionMethod.
 */
char
CompressionNameToMethod(const char *compression)
{
	if (strcmp(compression, "pglz") == 0)
		return TOAST_PGLZ_COMPRESSION;
	else if (strcmp(compression, "lz4") == 0)
	{
#ifndef USE_LZ4
		NO_METHOD_SUPPORT("lz4");
#endif
		return TOAST_LZ4_COMPRESSION;
	}
	else if (strcmp(compression, "zstd") == 0)
	{
#ifndef USE_ZSTD
		NO_METHOD_SUPPORT("zstd");
#endif
		return TOAST_ZSTD_COMPRESSION;
	}

	return InvalidCompressionMethod;
}

/*
 * GetCompressionMethodName - Get compression method name
 */
const char *
GetCompressionMethodName(char method)
{
	switch (method)
	{
		case TOAST_PGLZ_COMPRESSION:
			return "pglz";
		case TOAST_LZ4_COMPRESSION:
			return "lz4";
		case TOAST_ZSTD_COMPRESSION:
			return "zstd";
		default:
			elog(ERROR, "invalid compression method %c", method);
			return NULL;		/* keep compiler quiet */
	}
}
