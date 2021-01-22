/* Copyright (C) 2001-2020 Artifex Software, Inc.
   All Rights Reserved.

   This software is provided AS-IS with no warranty, either express or
   implied.

   This software is distributed under license and may not be copied,
   modified or distributed except as expressly authorized under the terms
   of the license contained in the file LICENSE in this distribution.

   Refer to licensing information at http://www.artifex.com or contact
   Artifex Software, Inc.,  1305 Grant Avenue - Suite 200, Novato,
   CA 94945, U.S.A., +1(415)492-9861, for further information.
*/

/* array handling for the PDF interpreter */

#include "ghostpdf.h"
#include "pdf_types.h"
#include "pdf_stack.h"
#include "pdf_deref.h"
#include "pdf_array.h"

/* NOTE: I think this should take a pdf_context param, but it's not available where it's
 * called, would require some surgery.
 */
void pdfi_free_array(pdf_obj *o)
{
    pdf_array *a = (pdf_array *)o;
    int i;

    for (i=0;i < a->size;i++) {
        if (a->values[i] != NULL)
            pdfi_countdown(a->values[i]);
    }
    gs_free_object(OBJ_MEMORY(a), a->values, "pdf interpreter free array contents");
    gs_free_object(OBJ_MEMORY(a), a, "pdf interpreter free array");
}

int pdfi_array_alloc(pdf_context *ctx, uint64_t size, pdf_array **a)
{
    int code, i;
    pdf_obj *n = NULL;

    *a = NULL;
    code = pdfi_alloc_object(ctx, PDF_ARRAY, size, (pdf_obj **)a);
    if (code < 0)
        return code;

    (*a)->size = size;

    if (size > 0) {
        /* Make a null object */
        code = pdfi_alloc_object(ctx, PDF_NULL, 1, &n);
        if (code < 0) {
            pdfi_countdown(*a);
            *a = NULL;
            return code;
        }
        /* And start all the array entries pointing at that null object.
         * array_put will replace tehm. This ensures we always have a valid
         * object for every entry. pdfi_array_from_stack() doesn't do this
         * initialisation because we know how many obejcts there are in the array
         * and we have valid objects for each entry on the stack already created.
         */
        for (i=0;i<size;i++){
            (*a)->values[i] = n;
            pdfi_countup(n);
        }
    }
    return 0;
}

/* This was defined in pdf_int.c until we moved the equivalent pdfi_dict_from_stack() into
 * pdf_dict.c, because we needed to be able to create dictionaries for images. We don't have
 * that need, but its less confusing to have the array_from_stack function defined in
 * here, similarly to the dictionary routine.
 */
int pdfi_array_from_stack(pdf_context *ctx, uint32_t indirect_num, uint32_t indirect_gen)
{
    uint64_t index = 0;
    pdf_array *a = NULL;
    pdf_obj *o;
    int code;

    code = pdfi_count_to_mark(ctx, &index);
    if (code < 0)
        return code;

    code = pdfi_array_alloc(ctx, index, &a);
    if (code < 0)
        return code;

    while (index) {
        o = ctx->stack_top[-1];
        code = pdfi_array_put(ctx, a, --index, o);
        if (code < 0) {
            (void)pdfi_clear_to_mark(ctx);
            return code;
        }
        pdfi_pop(ctx, 1);
    }

    code = pdfi_clear_to_mark(ctx);
    if (code < 0)
        return code;

    if (ctx->pdfdebug)
        dmprintf (ctx->memory, " ]\n");

    a->indirect_num = indirect_num;
    a->indirect_gen = indirect_gen;

    code = pdfi_push(ctx, (pdf_obj *)a);
    if (code < 0)
        pdfi_free_array((pdf_obj *)a);

    return code;
}

/* Fetch object from array, resolving indirect reference if needed
 * setref -- indicates whether to replace indirect ref with the object
 */
static int pdfi_array_fetch(pdf_context *ctx, pdf_array *a, uint64_t index, pdf_obj **o, bool setref)
{
    int code;
    pdf_obj *obj;

    *o = NULL;

    if (a->type != PDF_ARRAY)
        return_error(gs_error_typecheck);

    if (index >= a->size)
        return_error(gs_error_rangecheck);
    obj = a->values[index];

    if (obj->type == PDF_INDIRECT) {
        pdf_obj *o1 = NULL;
        pdf_indirect_ref *r = (pdf_indirect_ref *)obj;

        code = pdfi_deref_loop_detect(ctx, r->ref_object_num, r->ref_generation_num, &o1);
        if (code < 0)
            return code;

        if (setref)
            (void)pdfi_array_put(ctx, a, index, o1);
        obj = o1;
    } else {
        pdfi_countup(obj);
    }

    *o = obj;
    return 0;
}

/* The object returned by pdfi_array_get has its reference count incremented by 1 to
 * indicate the reference now held by the caller, in **o.
 */
int pdfi_array_get(pdf_context *ctx, pdf_array *a, uint64_t index, pdf_obj **o)
{
    int code;

    code = pdfi_array_fetch(ctx, a, index, o, true);
    if (code < 0) return code;

    return 0;
}

/* Get element from array without resolving PDF_INDIRECT dereferences.
 * It looks to me like some usages need to do the checking themselves to
 * avoid circular references?  Can remove this if not really needed.
 */
int pdfi_array_get_no_deref(pdf_context *ctx, pdf_array *a, uint64_t index, pdf_obj **o)
{
    if (a->type != PDF_ARRAY)
        return_error(gs_error_typecheck);

    if (index >= a->size)
        return_error(gs_error_rangecheck);

    *o = a->values[index];
    pdfi_countup(*o);
    return 0;
}

/* Same as pdfi_array_get() but doesn't replace indirect ref with a new object.
 */
int pdfi_array_get_no_store_R(pdf_context *ctx, pdf_array *a, uint64_t index, pdf_obj **o)
{
    int code;

    code = pdfi_array_fetch(ctx, a, index, o, false);
    if (code < 0) return code;

    return 0;
}

/* Get value from pdfi_array.
 * Handles type-checking and resolving indirect references.
 */
int pdfi_array_get_type(pdf_context *ctx, pdf_array *a, uint64_t index,
                    pdf_obj_type type, pdf_obj **o)
{
    int code;

    code = pdfi_array_get(ctx, a, index, o);
    if (code < 0)
        return code;

    if ((*o)->type != type) {
        pdfi_countdown(*o);
        *o = NULL;
        return_error(gs_error_typecheck);
    }
    return 0;
}

int pdfi_array_get_int(pdf_context *ctx, pdf_array *a, uint64_t index, int64_t *i)
{
    int code;
    pdf_num *n;

    code = pdfi_array_get_type(ctx, a, index, PDF_INT, (pdf_obj **)&n);
    if (code < 0)
        return code;
    *i = n->value.i;
    pdfi_countdown(n);
    return 0;
}

int pdfi_array_get_number(pdf_context *ctx, pdf_array *a, uint64_t index, double *f)
{
    int code;
    pdf_num *n;

    code = pdfi_array_get(ctx, a, index, (pdf_obj **)&n);
    if (code < 0)
        return code;

    if (n->type == PDF_INT)
        *f = (double)n->value.i;
    else {
        if (n->type == PDF_REAL)
            *f = n->value.d;
        else {
            code = gs_note_error(gs_error_typecheck);
        }
    }
    pdfi_countdown(n);

    return code;
}

/* Check whether a particular object is in an array.
 * If index is not NULL, fill it in with the index of the object.
 * Note that this will resolve indirect references if needed.
 */
bool pdfi_array_known(pdf_context *ctx, pdf_array *a, pdf_obj *o, int *index)
{
    int i;

    if (a->type != PDF_ARRAY)
        return_error(gs_error_typecheck);

    for (i=0; i < a->size; i++) {
        pdf_obj *val;
        int code;

        code = pdfi_array_fetch(ctx, a, i, &val, true);
        if (code < 0)
            continue;
        if (val->object_num == o->object_num) {
            if (index != NULL) *index = i;
            pdfi_countdown(val);
            return true;
        }
        pdfi_countdown(val);
    }
    return false;
}

int pdfi_array_put(pdf_context *ctx, pdf_array *a, uint64_t index, pdf_obj *o)
{
    if (a->type != PDF_ARRAY)
        return_error(gs_error_typecheck);

    if (index >= a->size)
        return_error(gs_error_rangecheck);

    pdfi_countdown(a->values[index]);
    a->values[index] = o;
    pdfi_countup(o);
    return 0;
}

int pdfi_array_put_int(pdf_context *ctx, pdf_array *a, uint64_t index, int64_t val)
{
    int code;
    pdf_num *obj;

    if (a->type != PDF_ARRAY)
        return_error(gs_error_typecheck);

    code = pdfi_alloc_object(ctx, PDF_INT, 0, (pdf_obj **)&obj);
    obj->value.i = val;
    if (code < 0)
        return code;

    return pdfi_array_put(ctx, a, index, (pdf_obj *)obj);
}

int pdfi_array_put_real(pdf_context *ctx, pdf_array *a, uint64_t index, double val)
{
    int code;
    pdf_num *obj;

    if (a->type != PDF_ARRAY)
        return_error(gs_error_typecheck);

    code = pdfi_alloc_object(ctx, PDF_REAL, 0, (pdf_obj **)&obj);
    obj->value.d = val;
    if (code < 0)
        return code;

    return pdfi_array_put(ctx, a, index, (pdf_obj *)obj);
}

/* Strictly speaking the normalize_rect isn't really part of the PDF array
 * processing, but its very likely that any time we want to use it, the
 * rectangle will have come from a PDF array in a PDF file so it makes
 * sense to have it here.
 */

/* Normalize rectangle */
void pdfi_normalize_rect(pdf_context *ctx, gs_rect *rect)
{
    double temp;

    /* Normalize the rectangle */
    if (rect->p.x > rect->q.x) {
        temp = rect->p.x;
        rect->p.x = rect->q.x;
        rect->q.x = temp;
    }
    if (rect->p.y > rect->q.y) {
        temp = rect->p.y;
        rect->p.y = rect->q.y;
        rect->q.y = temp;
    }
}

/*
 * Turn an Array into a gs_rect.  If Array is NULL, makes a tiny rect
 */
int pdfi_array_to_gs_rect(pdf_context *ctx, pdf_array *array, gs_rect *rect)
{
    double number;
    int code = 0;

    /* Init to tiny rect to allow sane continuation on errors */
    rect->p.x = 0.0;
    rect->p.y = 0.0;
    rect->q.x = 1.0;
    rect->q.y = 1.0;

    /* Identity matrix if no array */
    if (array == NULL || array->type != PDF_ARRAY) {
        return 0;
    }
    if (pdfi_array_size(array) != 4) {
        return_error(gs_error_rangecheck);
    }
    code = pdfi_array_get_number(ctx, array, 0, &number);
    if (code < 0) goto errorExit;
    rect->p.x = (float)number;
    code = pdfi_array_get_number(ctx, array, 1, &number);
    if (code < 0) goto errorExit;
    rect->p.y = (float)number;
    code = pdfi_array_get_number(ctx, array, 2, &number);
    if (code < 0) goto errorExit;
    rect->q.x = (float)number;
    code = pdfi_array_get_number(ctx, array, 3, &number);
    if (code < 0) goto errorExit;
    rect->q.y = (float)number;

    return 0;

 errorExit:
    return code;
}

/* Turn a /Matrix Array into a gs_matrix.  If Array is NULL, makes an identity matrix */
int pdfi_array_to_gs_matrix(pdf_context *ctx, pdf_array *array, gs_matrix *mat)
{
    double number;
    int code = 0;

    /* Init to identity matrix to allow sane continuation on errors */
    mat->xx = 1.0;
    mat->xy = 0.0;
    mat->yx = 0.0;
    mat->yy = 1.0;
    mat->tx = 0.0;
    mat->ty = 0.0;

    /* Identity matrix if no array */
    if (array == NULL || array->type != PDF_ARRAY) {
        return 0;
    }
    if (pdfi_array_size(array) != 6) {
        return_error(gs_error_rangecheck);
    }
    code = pdfi_array_get_number(ctx, array, 0, &number);
    if (code < 0) goto errorExit;
    mat->xx = (float)number;
    code = pdfi_array_get_number(ctx, array, 1, &number);
    if (code < 0) goto errorExit;
    mat->xy = (float)number;
    code = pdfi_array_get_number(ctx, array, 2, &number);
    if (code < 0) goto errorExit;
    mat->yx = (float)number;
    code = pdfi_array_get_number(ctx, array, 3, &number);
    if (code < 0) goto errorExit;
    mat->yy = (float)number;
    code = pdfi_array_get_number(ctx, array, 4, &number);
    if (code < 0) goto errorExit;
    mat->tx = (float)number;
    code = pdfi_array_get_number(ctx, array, 5, &number);
    if (code < 0) goto errorExit;
    mat->ty = (float)number;
    return 0;

 errorExit:
    return code;
}

/* Turn a pdf_array into a double array of specified size */
int pdfi_array_to_num_array(pdf_context *ctx, pdf_array *array, double *out, int offset, int size)
{
    int i;
    int code;
    double num;

    for (i=0; i<size; i++) {
        code = pdfi_array_get_number(ctx, array, offset+i, &num);
        if (code < 0)
            return code;
        out[i] = num;
    }
    return 0;
}

/* Transform a BBox by a matrix (from zmatrix.c/zbbox_transform())*/
void
pdfi_bbox_transform(pdf_context *ctx, gs_rect *bbox, gs_matrix *matrix)
{
    gs_point aa, az, za, zz;
    double temp;

    gs_point_transform(bbox->p.x, bbox->p.y, matrix, &aa);
    gs_point_transform(bbox->p.x, bbox->q.y, matrix, &az);
    gs_point_transform(bbox->q.x, bbox->p.y, matrix, &za);
    gs_point_transform(bbox->q.x, bbox->q.y, matrix, &zz);

    if ( aa.x > az.x)
        temp = aa.x, aa.x = az.x, az.x = temp;
    if ( za.x > zz.x)
        temp = za.x, za.x = zz.x, zz.x = temp;
    if ( za.x < aa.x)
        aa.x = za.x;  /* min */
    if ( az.x > zz.x)
        zz.x = az.x;  /* max */

    if ( aa.y > az.y)
        temp = aa.y, aa.y = az.y, az.y = temp;
    if ( za.y > zz.y)
        temp = za.y, za.y = zz.y, zz.y = temp;
    if ( za.y < aa.y)
        aa.y = za.y;  /* min */
    if ( az.y > zz.y)
        zz.y = az.y;  /* max */

    bbox->p.x = aa.x;
    bbox->p.y = aa.y;
    bbox->q.x = zz.x;
    bbox->q.y = zz.y;
}
