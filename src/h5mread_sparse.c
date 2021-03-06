/****************************************************************************
 *                    Workhorse behind h5mread method 8                     *
 *                            Author: H. Pag\`es                            *
 ****************************************************************************/
#include "h5mread_sparse.h"

#include "global_errmsg_buf.h"
#include "uaselection.h"
#include "h5mread_helpers.h"

#include <stdlib.h>  /* for malloc, free */
#include <string.h>  /* for memcpy */
#include <limits.h>  /* for INT_MAX */
//#include <time.h>


/****************************************************************************
 * Fast append an element to an auto-extending buffer
 */

static inline void IntAE_fast_append(IntAE *ae, int val)
{
	/* We don't use IntAE_get_nelt() for maximum speed. */
	if (ae->_nelt == ae->_buflength)
		IntAE_extend(ae, increase_buflength(ae->_buflength));
	ae->elts[ae->_nelt++] = val;
	return;
}

static inline void DoubleAE_fast_append(DoubleAE *ae, double val)
{
	/* We don't use DoubleAE_get_nelt() for maximum speed. */
	if (ae->_nelt == ae->_buflength)
		DoubleAE_extend(ae, increase_buflength(ae->_buflength));
	ae->elts[ae->_nelt++] = val;
	return;
}

static inline void CharAE_fast_append(CharAE *ae, char c)
{
	/* We don't use CharAE_get_nelt() for maximum speed. */
	if (ae->_nelt == ae->_buflength)
		CharAE_extend(ae, increase_buflength(ae->_buflength));
	ae->elts[ae->_nelt++] = c;
	return;
}

static inline void CharAEAE_fast_append(CharAEAE *aeae, CharAE *ae)
{
	/* We don't use CharAEAE_get_nelt() for maximum speed. */
	if (aeae->_nelt == aeae->_buflength)
		CharAEAE_extend(aeae, increase_buflength(aeae->_buflength));
	aeae->elts[aeae->_nelt++] = ae;
	return;
}


/****************************************************************************
 * Fast append a non-zero value to an auto-extending buffer
 *
 * These helpers functions are used in append_nonzero_val_to_nzdata_buf().
 * Note that we cap both the length of 'nzdata' and the number of rows in
 * 'nzindex' to INT_MAX. This is to prevent 'nzindex' from growing into a
 * matrix with more than INT_MAX rows, which R does not support yet.
 */

#define	NZDATA_MAXLENGTH INT_MAX

static inline int IntAE_append_if_nonzero(IntAE *ae, int val)
{
	if (val == 0)
		return 0;
	/* We don't use IntAE_get_nelt() for maximum speed. */
	if (ae->_nelt >= NZDATA_MAXLENGTH)
		return -1;
	IntAE_fast_append(ae, val);
	return 1;
}

static inline int DoubleAE_append_if_nonzero(DoubleAE *ae, double val)
{
	if (val == 0.0)
		return 0;
	/* We don't use DoubleAE_get_nelt() for maximum speed. */
	if (ae->_nelt >= NZDATA_MAXLENGTH)
		return -1;
	DoubleAE_fast_append(ae, val);
	return 1;
}

static inline int CharAE_append_if_nonzero(CharAE *ae, char c)
{
	if (c == 0)
		return 0;
	/* We don't use CharAE_get_nelt() for maximum speed. */
	if (ae->_nelt >= NZDATA_MAXLENGTH)
		return -1;
	CharAE_fast_append(ae, c);
	return 1;
}

static inline int CharAEAE_append_if_nonzero(CharAEAE *aeae, const char *s,
					     size_t n)
{
	size_t s_len;

	for (s_len = 0; s_len < n; s_len++)
		if (s[s_len] == 0)
			break;
	if (s_len == 0)
		return 0;
	/* We don't use CharAEAE_get_nelt() for maximum speed. */
	if (aeae->_nelt >= NZDATA_MAXLENGTH)
		return -1;
	CharAE *ae = new_CharAE(s_len);
	memcpy(ae->elts, s, s_len);
	/* We don't use CharAE_set_nelt() for maximum speed. */
	ae->_nelt = s_len;
	CharAEAE_fast_append(aeae, ae);
	return 1;
}


/****************************************************************************
 * Manipulation of the 'nzindex' and 'nzdata' buffers
 */

static void *new_nzdata_buf(SEXPTYPE Rtype)
{
	switch (Rtype) {
	    case LGLSXP: case INTSXP: return new_IntAE(0, 0, 0);
	    case REALSXP:             return new_DoubleAE(0, 0, 0.0);
	    case STRSXP:              return new_CharAEAE(0, 0);
	    case RAWSXP:              return new_CharAE(0);
	}
	/* Should never happen. The early call to _init_H5DSetDescriptor()
	   in h5mread() already made sure that Rtype is supported. */
	PRINT_TO_ERRMSG_BUF("unsupported type: %s", CHAR(type2str(Rtype)));
	return NULL;
}

static SEXP make_nzindex_from_bufs(const IntAEAE *nzindex_bufs)
{
	int ndim, along;
	size_t nzindex_nrow;
	SEXP nzindex;
	int *out_p;

	ndim = IntAEAE_get_nelt(nzindex_bufs);
	nzindex_nrow = IntAE_get_nelt(nzindex_bufs->elts[0]);
	/* 'nzindex_nrow' is guaranteed to be <= INT_MAX (see NZDATA_MAXLENGTH
	   above) otherwise earlier calls to append_nonzero_val_to_nzdata_buf()
	   (see below) would have raised an error. */
	nzindex = PROTECT(allocMatrix(INTSXP, (int) nzindex_nrow, ndim));
	out_p = INTEGER(nzindex);
	for (along = 0; along < ndim; along++) {
		memcpy(out_p, nzindex_bufs->elts[along]->elts,
		       sizeof(int) * nzindex_nrow);
		out_p += nzindex_nrow;
	}
	UNPROTECT(1);
	return nzindex;
}

static SEXP make_nzdata_from_buf(const void *nzdata_buf, SEXPTYPE Rtype)
{
	switch (Rtype) {
	    case LGLSXP:  return new_LOGICAL_from_IntAE(nzdata_buf);
	    case INTSXP:  return new_INTEGER_from_IntAE(nzdata_buf);
	    case REALSXP: return new_NUMERIC_from_DoubleAE(nzdata_buf);
	    case STRSXP:  return new_CHARACTER_from_CharAEAE(nzdata_buf);
	    case RAWSXP:  return new_RAW_from_CharAE(nzdata_buf);
	}
	/* Should never happen. The early call to _init_H5DSetDescriptor()
	   in h5mread() already made sure that Rtype is supported. */
	PRINT_TO_ERRMSG_BUF("unsupported type: %s", CHAR(type2str(Rtype)));
	return R_NilValue;
}

/* No longer used. Replaced by make_nzindex_from_bufs() above. */
static SEXP NOT_USED_make_nzindex_from_buf(const IntAE *nzindex_buf,
		R_xlen_t nzdata_len, int ndim)
{
	SEXP nzindex;
	size_t nzindex_len;
	const int *in_p;
	int i, along;

	nzindex_len = IntAE_get_nelt(nzindex_buf);
	if (nzindex_len != (size_t) nzdata_len * ndim) {
		/* Should never happen. */
		PRINT_TO_ERRMSG_BUF("HDF5Array internal error in "
				    "C function make_nzindex_from_buf(): "
				    "length(nzindex) != length(nzdata) * ndim");
		return R_NilValue;
	}
	/* 'nzindex_nrow' is guaranteed to be <= INT_MAX (see NZDATA_MAXLENGTH
	   above) otherwise earlier calls to append_nonzero_val_to_nzdata_buf()
	   (see below) would have raised an error. */
	nzindex = PROTECT(allocMatrix(INTSXP, (int) nzdata_len, ndim));
	in_p = nzindex_buf->elts;
	for (i = 0; i < nzdata_len; i++) {
		for (along = 0; along < ndim; along++) {
			INTEGER(nzindex)[i + nzdata_len * along] = *(in_p++);
		}
	}
	UNPROTECT(1);
	return nzindex;
}

static int copy_nzindex_and_nzdata_to_ans(const H5DSetDescriptor *h5dset,
		const IntAEAE *nzindex_bufs, const void *nzdata_buf, SEXP ans)
{
	SEXP ans_elt;

	/* Move the data in 'nzindex_bufs' to an ordinary matrix. */
	ans_elt = PROTECT(make_nzindex_from_bufs(nzindex_bufs));
	SET_VECTOR_ELT(ans, 0, ans_elt);
	UNPROTECT(1);
	if (ans_elt == R_NilValue)  /* should never happen */
		return -1;
	/* Move the data in 'nzdata_buf' to an atomic vector. */
	ans_elt = PROTECT(make_nzdata_from_buf(nzdata_buf, h5dset->Rtype));
	SET_VECTOR_ELT(ans, 1, ans_elt);
	UNPROTECT(1);
	if (ans_elt == R_NilValue)  /* should never happen */
		return -1;
	return 0;
}

/* Replacements for make_nzdata_from_buf() and NOT_USED_make_nzindex_from_buf()
   in case we decided to use one nzdata and one nzindex buffer per touched
   chunk (e.g. an IntAEAE of length the total nb of touched chunks) instead
   of a single nzdata and nzindex buffer for all the chunks like we do at the
   moment. */
static SEXP NOT_USED_make_nzdata_from_IntAE_bufs(const IntAEAE *nzdata_bufs,
		SEXPTYPE Rtype)
{
	size_t total_num_tchunks, nzdata_len, tchunk_rank, buf_nelt;
	const IntAE *buf;
	SEXP nzdata;
	int *out_p;

	total_num_tchunks = IntAEAE_get_nelt(nzdata_bufs);
	nzdata_len = 0;
	for (tchunk_rank = 0; tchunk_rank < total_num_tchunks; tchunk_rank++) {
		buf = nzdata_bufs->elts[tchunk_rank];
		buf_nelt = IntAE_get_nelt(buf);
		nzdata_len += buf_nelt;
	}
	nzdata = PROTECT(allocVector(Rtype, nzdata_len));
	out_p = (int *) DATAPTR(nzdata);
	for (tchunk_rank = 0; tchunk_rank < total_num_tchunks; tchunk_rank++) {
		buf = nzdata_bufs->elts[tchunk_rank];
		buf_nelt = IntAE_get_nelt(buf);
		memcpy(out_p, buf->elts, sizeof(int) * buf_nelt);
		out_p += buf_nelt;
	}
	UNPROTECT(1);
	return nzdata;
}

static SEXP NOT_USED_make_nzindex_from_bufs(const IntAEAE *nzindex_bufs,
		R_xlen_t nzdata_len, int ndim)
{
	size_t total_num_tchunks, tchunk_rank, buf_nelt;
	const IntAE *buf;
	SEXP nzindex;
	int *out_p, i, buf_nrow, along;
	const int *in_p;

	/* 'nzindex_nrow' is guaranteed to be <= INT_MAX (see NZDATA_MAXLENGTH
	   above) otherwise earlier calls to append_nonzero_val_to_nzdata_buf()
	   (see below) would have raised an error. */
	nzindex = PROTECT(allocMatrix(INTSXP, (int) nzdata_len, ndim));
	out_p = INTEGER(nzindex);
	total_num_tchunks = IntAEAE_get_nelt(nzindex_bufs);
	for (tchunk_rank = 0; tchunk_rank < total_num_tchunks; tchunk_rank++) {
		buf = nzindex_bufs->elts[tchunk_rank];
		buf_nelt = IntAE_get_nelt(buf);
		buf_nrow = buf_nelt / ndim;
		in_p = buf->elts;
		for (i = 0; i < buf_nrow; i++) {
			for (along = 0; along < ndim; along++)
				out_p[nzdata_len * along] = *(in_p++);
			out_p++;
		}
	}
	UNPROTECT(1);
	return nzindex;
}


/****************************************************************************
 * Low-level helpers used by the data gathering functions
 */

static void init_in_offset(int ndim, SEXP starts,
		const hsize_t *h5chunkdim, const H5Viewport *dest_vp,
		const H5Viewport *tchunk_vp,
		size_t *in_offset)
{
	size_t in_off;
	int along, h5along, i;
	SEXP start;

	in_off = 0;
	for (along = ndim - 1, h5along = 0; along >= 0; along--, h5along++) {
		in_off *= h5chunkdim[h5along];
		i = dest_vp->off[along];
		start = GET_LIST_ELT(starts, along);
		if (start != R_NilValue)
			in_off += _get_trusted_elt(start, i) - 1 -
				  tchunk_vp->h5off[h5along];
	}
	*in_offset = in_off;
	return;
}

static inline void update_in_offset(int ndim, SEXP starts,
		const hsize_t *h5chunkdim, const H5Viewport *dest_vp,
		const int *inner_midx, int inner_moved_along,
		size_t *in_offset)
{
	SEXP start;
	int i1, i0, along, h5along, di;
	long long int in_off_inc;

	start = GET_LIST_ELT(starts, inner_moved_along);
	if (start != R_NilValue) {
		i1 = dest_vp->off[inner_moved_along] +
		     inner_midx[inner_moved_along];
		i0 = i1 - 1;
		in_off_inc = _get_trusted_elt(start, i1) -
			     _get_trusted_elt(start, i0);
	} else {
		in_off_inc = 1;
	}
	if (inner_moved_along >= 1) {
		along = inner_moved_along - 1;
		h5along = ndim - inner_moved_along;
		do {
			in_off_inc *= h5chunkdim[h5along];
			di = 1 - dest_vp->dim[along];
			start = GET_LIST_ELT(starts, along);
			if (start != R_NilValue) {
				i1 = dest_vp->off[along];
				i0 = i1 - di;
				in_off_inc += _get_trusted_elt(start, i1) -
					      _get_trusted_elt(start, i0);
			} else {
				in_off_inc += di;
			}
			along--;
			h5along++;
		} while (along >= 0);
	}
	*in_offset += in_off_inc;
	return;
}

/* We don't let the length of 'nzdata' exceed INT_MAX (see NZDATA_MAXLENGTH
   above). Return 0 if val is zero, 1 if val is non-zero and was successfully
   appended, and -1 if val is non-zero but couldn't be appended because the
   length of 'nzdata' is already NZDATA_MAXLENGTH. */
static inline int append_nonzero_val_to_nzdata_buf(
		const H5DSetDescriptor *h5dset,
		const int *in, size_t in_offset,
		void *nzdata_buf)
{
	int ret;

	switch (h5dset->Rtype) {
	    case LGLSXP:
	    case INTSXP: {
		int val = ((int *) in)[in_offset];
		ret = IntAE_append_if_nonzero((IntAE *) nzdata_buf, val);
	    } break;
	    case REALSXP: {
		double val = ((double *) in)[in_offset];
		ret = DoubleAE_append_if_nonzero((DoubleAE *) nzdata_buf, val);
	    } break;
	    case STRSXP: {
		const char *s = ((char *) in) + in_offset * h5dset->H5size;
		ret = CharAEAE_append_if_nonzero((CharAEAE *) nzdata_buf, s,
						 h5dset->H5size);
	    } break;
	    case RAWSXP: {
		char val = ((char *) in)[in_offset];
		ret = CharAE_append_if_nonzero((CharAE *) nzdata_buf, val);
	    } break;
	    default:
		PRINT_TO_ERRMSG_BUF("unsupported type: %s",
				    CHAR(type2str(h5dset->Rtype)));
		return -1;
	}
	if (ret < 0)
		PRINT_TO_ERRMSG_BUF("too many non-zero values to load");
	return ret;
}

static inline void append_array_index_to_nzindex_bufs(
		const H5Viewport *dest_vp,
		const int *inner_midx_buf,
		IntAEAE *nzindex_bufs)
{
	int ndim, along, midx;
	IntAE *nzindex_buf;

	/* We don't use IntAEAE_get_nelt() for maximum speed. */
	ndim = nzindex_bufs->_nelt;
	for (along = 0; along < ndim; along++) {
		nzindex_buf = nzindex_bufs->elts[along];
		midx = dest_vp->off[along] + inner_midx_buf[along] + 1;
		IntAE_fast_append(nzindex_buf, midx);
	}
	return;
}


/****************************************************************************
 * Data gathering functions
 */

typedef int (*GatherChunkDataFunType)(
		const H5DSetDescriptor *h5dset, SEXP starts,
		const void *chunk_data_buf, const H5Viewport *tchunk_vp,
		const H5Viewport *dest_vp, int *inner_midx_buf,
		IntAEAE *nzindex_bufs, void *nzdata_buf);

/* Does NOT work properly on a truncated chunk! Works properly only if the
   chunk data fills the full 'chunk_data_buf', that is, if the current chunk
   is a full-size chunk and not a "truncated" chunk (a.k.a. "partial edge
   chunk" in HDF5's terminology). */
static int gather_full_chunk_data_as_sparse(
		const H5DSetDescriptor *h5dset, SEXP starts,
		const void *in, const H5Viewport *tchunk_vp,
		const H5Viewport *dest_vp, int *inner_midx_buf,
		IntAEAE *nzindex_bufs, void *nzdata_buf)
{
	int ndim, inner_moved_along, ret;
	size_t in_offset;

	ndim = h5dset->ndim;
	in_offset = 0;
	/* Walk on **all** the elements in current chunk and append
	   the non-zero ones to 'nzindex_bufs' and 'nzdata_buf'. */
	while (1) {
		ret = append_nonzero_val_to_nzdata_buf(h5dset,
					in, in_offset, nzdata_buf);
		if (ret < 0)
			return -1;
		if (ret > 0)
			append_array_index_to_nzindex_bufs(dest_vp,
					inner_midx_buf, nzindex_bufs);
		inner_moved_along = _next_midx(ndim, dest_vp->dim,
					       inner_midx_buf);
		if (inner_moved_along == ndim)
			break;
		in_offset++;
	};
	return 0;
}

static int gather_selected_chunk_data_as_sparse(
		const H5DSetDescriptor *h5dset, SEXP starts,
		const void *in, const H5Viewport *tchunk_vp,
		const H5Viewport *dest_vp, int *inner_midx_buf,
		IntAEAE *nzindex_bufs, void *nzdata_buf)
{
	int ndim, inner_moved_along, ret;
	size_t in_offset;

	ndim = h5dset->ndim;
	init_in_offset(ndim, starts, h5dset->h5chunkdim, dest_vp,
		       tchunk_vp,
		       &in_offset);
	/* Walk on the **selected** elements in current chunk and append
	   the non-zero ones to 'nzindex_bufs' and 'nzdata_buf'. */
	while (1) {
		ret = append_nonzero_val_to_nzdata_buf(h5dset,
					in, in_offset, nzdata_buf);
		if (ret < 0)
			return -1;
		if (ret > 0)
			append_array_index_to_nzindex_bufs(dest_vp,
					inner_midx_buf, nzindex_bufs);
		inner_moved_along = _next_midx(ndim, dest_vp->dim,
					       inner_midx_buf);
		if (inner_moved_along == ndim)
			break;
		update_in_offset(ndim, starts, h5dset->h5chunkdim, dest_vp,
				 inner_midx_buf, inner_moved_along,
				 &in_offset);
	};
	return 0;
}

static int gather_chunk_data_as_sparse(
		const H5DSetDescriptor *h5dset, SEXP starts,
		const void *chunk_data_buf, const H5Viewport *tchunk_vp,
		const H5Viewport *dest_vp, int *inner_midx_buf,
		IntAEAE *nzindex_bufs, void *nzdata_buf)
{
	int go_fast, ret;

	go_fast = _tchunk_is_fully_selected(h5dset->ndim, tchunk_vp, dest_vp)
		  && ! _tchunk_is_truncated(h5dset, tchunk_vp);
	if (go_fast) {
		ret = gather_full_chunk_data_as_sparse(
			h5dset, starts,
			chunk_data_buf, tchunk_vp,
			dest_vp, inner_midx_buf,
			nzindex_bufs, nzdata_buf);
	} else {
		ret = gather_selected_chunk_data_as_sparse(
			h5dset, starts,
			chunk_data_buf, tchunk_vp,
			dest_vp, inner_midx_buf,
			nzindex_bufs, nzdata_buf);
	}
	return ret;
}

/* Does NOT work properly on a truncated chunk! Works properly only if the
   chunk data fills the full 'chunk_data_buf', that is, if the current chunk
   is a full-size chunk and not a "truncated" chunk (a.k.a. "partial edge
   chunk" in HDF5's terminology). */
static int gather_full_chunk_int_data_as_sparse(
		const H5DSetDescriptor *h5dset, SEXP starts,
		const int *in, const H5Viewport *tchunk_vp,
		const H5Viewport *dest_vp, int *inner_midx_buf,
		IntAEAE *nzindex_bufs, IntAE *nzdata_buf)
{
	int ndim, inner_moved_along, ret;

	ndim = h5dset->ndim;
	/* Walk on **all** the elements in current chunk and append
	   the non-zero ones to 'nzindex_bufs' and 'nzdata_buf'. */
	while (1) {
		ret = IntAE_append_if_nonzero(nzdata_buf, *in);
		if (ret < 0) {
			PRINT_TO_ERRMSG_BUF("too many non-zero values to load");
			return -1;
		}
		if (ret > 0)
			append_array_index_to_nzindex_bufs(dest_vp,
					inner_midx_buf, nzindex_bufs);
		inner_moved_along = _next_midx(ndim, dest_vp->dim,
					       inner_midx_buf);
		if (inner_moved_along == ndim)
			break;
		in++;
	};
	return 0;
}

static int gather_selected_chunk_int_data_as_sparse(
		const H5DSetDescriptor *h5dset, SEXP starts,
		const int *in, const H5Viewport *tchunk_vp,
		const H5Viewport *dest_vp, int *inner_midx_buf,
		IntAEAE *nzindex_bufs, IntAE *nzdata_buf)
{
	int ndim, inner_moved_along, ret;
	size_t in_offset;

	ndim = h5dset->ndim;
	init_in_offset(ndim, starts, h5dset->h5chunkdim, dest_vp,
		       tchunk_vp,
		       &in_offset);
	/* Walk on the **selected** elements in current chunk and append
	   the non-zero ones to 'nzindex_bufs' and 'nzdata_buf'. */
	while (1) {
		ret = IntAE_append_if_nonzero(nzdata_buf, in[in_offset]);
		if (ret < 0) {
			PRINT_TO_ERRMSG_BUF("too many non-zero values to load");
			return -1;
		}
		if (ret > 0)
			append_array_index_to_nzindex_bufs(dest_vp,
					inner_midx_buf, nzindex_bufs);
		inner_moved_along = _next_midx(ndim, dest_vp->dim,
					       inner_midx_buf);
		if (inner_moved_along == ndim)
			break;
		update_in_offset(ndim, starts, h5dset->h5chunkdim, dest_vp,
				 inner_midx_buf, inner_moved_along,
				 &in_offset);
	};
	return 0;
}

static int gather_chunk_int_data_as_sparse(
		const H5DSetDescriptor *h5dset, SEXP starts,
		const void *chunk_data_buf, const H5Viewport *tchunk_vp,
		const H5Viewport *dest_vp, int *inner_midx_buf,
		IntAEAE *nzindex_bufs, void *nzdata_buf)
{
	int go_fast, ret;

	go_fast = _tchunk_is_fully_selected(h5dset->ndim, tchunk_vp, dest_vp)
		  && ! _tchunk_is_truncated(h5dset, tchunk_vp);
	if (go_fast) {
		ret = gather_full_chunk_int_data_as_sparse(
			h5dset, starts,
			(const int *) chunk_data_buf, tchunk_vp,
			dest_vp, inner_midx_buf,
			nzindex_bufs, (IntAE *) nzdata_buf);
	} else {
		ret = gather_selected_chunk_int_data_as_sparse(
			h5dset, starts,
			(const int *) chunk_data_buf, tchunk_vp,
			dest_vp, inner_midx_buf,
			nzindex_bufs, (IntAE *) nzdata_buf);
	}
	return ret;
}

typedef struct sparse_data_gatherer_t {
	GatherChunkDataFunType gathering_fun;
	IntAEAE *nzindex_bufs;
	void *nzdata_buf;
} SparseDataGatherer;

static SparseDataGatherer sparse_data_gatherer(
		const H5DSetDescriptor *h5dset,
		IntAEAE *nzindex_bufs, void *nzdata_buf)
{
	SparseDataGatherer gatherer;

	/* INTSXP is the most common Rtype for sparse data so we give it a
	   little boost. */
	if (h5dset->Rtype == INTSXP || h5dset->Rtype == LGLSXP) {
		gatherer.gathering_fun = gather_chunk_int_data_as_sparse;
	} else {
		gatherer.gathering_fun = gather_chunk_data_as_sparse;
	}
	gatherer.nzindex_bufs = nzindex_bufs;
	gatherer.nzdata_buf = nzdata_buf;
	return gatherer;
}


/****************************************************************************
 * read_data_8()
 *
 * One call to _read_H5Viewport() (wrapper for H5Dread()) per chunk touched
 * by the user-supplied array selection.
 *
 * More precisely, walk over the chunks touched by 'starts'. For each chunk:
 *   - Make one call to _read_H5Viewport() to load the **entire** chunk data
 *     to an intermediate buffer.
 *   - Gather the non-zero user-selected data found in the chunk into
 *     'nzindex_bufs' and 'nzdata_buf'.
 *
 * Assumes that 'h5dset->h5chunkdim' and 'h5dset->h5nchunk' are NOT
 * NULL. This is NOT checked!
 */

static int read_data_8(const H5DSetDescriptor *h5dset,
		SEXP starts,
		const IntAEAE *breakpoint_bufs,
		const LLongAEAE *tchunkidx_bufs,
		const int *num_tchunks,
		IntAEAE *nzindex_bufs, void *nzdata_buf)
{
	int ndim, moved_along, ret;
	IntAE *tchunk_midx_buf, *inner_midx_buf;
	void *chunk_data_buf; //*compressed_chunk_data_buf;
	hid_t chunk_space_id;
	H5Viewport tchunk_vp, middle_vp, dest_vp;
	SparseDataGatherer gatherer;
	long long int tchunk_rank;

	ndim = h5dset->ndim;

	/* Prepare buffers. */

	tchunk_midx_buf = new_IntAE(ndim, ndim, 0);
	inner_midx_buf = new_IntAE(ndim, ndim, 0);

	chunk_data_buf = malloc(h5dset->chunk_data_buf_size);
	//chunk_data_buf = malloc(2 * h5dset->chunk_data_buf_size +
	//			CHUNK_COMPRESSION_OVERHEAD);
	if (chunk_data_buf == NULL) {
		PRINT_TO_ERRMSG_BUF("failed to allocate memory "
				    "for 'chunk_data_buf'");
		return -1;
	}
	//compressed_chunk_data_buf = chunk_data_buf +
	//			    h5dset->chunk_data_buf_size;
	chunk_space_id = H5Screate_simple(ndim, h5dset->h5chunkdim, NULL);
	if (chunk_space_id < 0) {
		free(chunk_data_buf);
		PRINT_TO_ERRMSG_BUF("H5Screate_simple() returned an error");
		return -1;
	}

	/* Allocate 'tchunk_vp', 'middle_vp', and 'dest_vp'.
	   We set 'dest_vp_mode' to ALLOC_OFF_AND_DIM because, in the
	   context of read_data_8(), we won't use 'dest_vp.h5off' or
	   'dest_vp.h5dim', only 'dest_vp.off' and 'dest_vp.dim'. */
	if (_alloc_tchunk_vp_middle_vp_dest_vp(ndim,
		&tchunk_vp, &middle_vp, &dest_vp,
		ALLOC_OFF_AND_DIM) < 0)
	{
		H5Sclose(chunk_space_id);
		free(chunk_data_buf);
		return -1;
	}

	gatherer = sparse_data_gatherer(h5dset, nzindex_bufs, nzdata_buf);

	/* Walk over the chunks touched by the user-supplied array selection. */
	tchunk_rank = 0;
	moved_along = ndim;
	do {
		_update_tchunk_vp_dest_vp(h5dset,
				tchunk_midx_buf->elts, moved_along,
				starts, breakpoint_bufs, tchunkidx_bufs,
				&tchunk_vp, &dest_vp);
		ret = _read_H5Viewport(h5dset,
				&tchunk_vp, &middle_vp,
				chunk_data_buf, chunk_space_id);
		//ret = _read_h5chunk(h5dset,
		//		&tchunk_vp,
		//		compressed_chunk_data_buf, chunk_data_buf);
		if (ret < 0)
			break;
		ret = gatherer.gathering_fun(h5dset, starts,
				chunk_data_buf, &tchunk_vp,
				//compressed_chunk_data_buf, &tchunk_vp,
				&dest_vp, inner_midx_buf->elts,
				gatherer.nzindex_bufs, gatherer.nzdata_buf);
		if (ret < 0)
			break;
		tchunk_rank++;
		moved_along = _next_midx(ndim, num_tchunks,
					 tchunk_midx_buf->elts);
	} while (moved_along < ndim);
	_free_tchunk_vp_middle_vp_dest_vp(&tchunk_vp, &middle_vp, &dest_vp);
	H5Sclose(chunk_space_id);
	free(chunk_data_buf);
	return ret;
}


/****************************************************************************
 * _h5mread_sparse()
 *
 * Implements method 8.
 * Return 'list(nzindex, nzdata, NULL)' or R_NilValue if an error occured.
 */

SEXP _h5mread_sparse(const H5DSetDescriptor *h5dset, SEXP starts, int *ans_dim)
{
	int ndim, ret;
	IntAEAE *breakpoint_bufs, *nzindex_bufs;
	LLongAEAE *tchunkidx_bufs;  /* touched chunk ids along each dim */
	IntAE *ntchunk_buf;  /* nb of touched chunks along each dim */
	long long int total_num_tchunks;
	void *nzdata_buf;

	SEXP ans;

	ndim = h5dset->ndim;

	/* This call will populate 'ans_dim', 'breakpoint_bufs',
	   and 'tchunkidx_bufs'. */
	breakpoint_bufs = new_IntAEAE(ndim, ndim);
	tchunkidx_bufs = new_LLongAEAE(ndim, ndim);
	ret = _map_starts_to_h5chunks(h5dset, starts, ans_dim,
				      breakpoint_bufs, tchunkidx_bufs);
	if (ret < 0)
		return R_NilValue;

	ntchunk_buf = new_IntAE(ndim, ndim, 0);
	total_num_tchunks = _set_num_tchunks(h5dset, starts,
					     tchunkidx_bufs, ntchunk_buf->elts);

	nzindex_bufs = new_IntAEAE(ndim, ndim);
	nzdata_buf = new_nzdata_buf(h5dset->Rtype);
	if (nzdata_buf == NULL)  /* should never happen */
		return R_NilValue;

	/* total_num_tchunks != 0 means that the user-supplied array selection
	   is not empty */
	if (total_num_tchunks != 0) {
		ret = read_data_8(h5dset, starts,
				  breakpoint_bufs, tchunkidx_bufs,
				  ntchunk_buf->elts,
				  nzindex_bufs, nzdata_buf);
		if (ret < 0)
			return R_NilValue;
	}

	ans = PROTECT(NEW_LIST(3));
	//clock_t t0 = clock();
	ret = copy_nzindex_and_nzdata_to_ans(h5dset, nzindex_bufs, nzdata_buf,
					     ans);
	UNPROTECT(1);
	if (ret < 0)
		return R_NilValue;
	//double dt = (1.0 * clock() - t0) / CLOCKS_PER_SEC;
	//printf("copy_nzindex_and_nzdata_to_ans(): %2.3f s\n", dt);
	return ans;
}

