#include "HDF5Array.h"
#include <R_ext/Rdynload.h>

#define CALLMETHOD_DEF(fun, numArgs) {#fun, (DL_FUNC) &fun, numArgs}

static const R_CallMethodDef callMethods[] = {

/* h5mread.c */
	CALLMETHOD_DEF(C_reduce_starts, 1),
	CALLMETHOD_DEF(C_h5mread, 4),

	{NULL, NULL, 0}
};


void R_init_HDF5Array(DllInfo *info)
{
	R_registerRoutines(info, NULL, callMethods, NULL, NULL);

	return;
}

