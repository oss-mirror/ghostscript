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

/* pdfmark handling for the PDF interpreter */

#include "pdf_int.h"
#include "plmain.h"
#include "pdf_stack.h"
#include "pdf_file.h"
#include "pdf_dict.h"
#include "pdf_array.h"
#include "pdf_loop_detect.h"
#include "pdf_mark.h"
#include "pdf_obj.h"
#include "pdf_misc.h"
#include "pdf_page.h"
#include "pdf_deref.h"

static int pdfi_mark_setparam_obj(pdf_context *ctx, pdf_obj *obj, gs_param_string *entry)
{
    int code = 0;
    byte *data = NULL;
    int size = 0;

    code = pdfi_obj_to_string(ctx, obj, &data, &size);
    if (code < 0)
        return code;
    entry->data = data;
    entry->size = size;
    entry->persistent = false;
    return 0;
}

static int pdfi_mark_setparam_pair(pdf_context *ctx, pdf_name *Key, pdf_obj *Value,
                                   gs_param_string *entry)
{
    int code = 0;

    /* Handle the Key */
    if (Key->type != PDF_NAME) {
        code = gs_note_error(gs_error_typecheck);
        goto exit;
    }

    code = pdfi_mark_setparam_obj(ctx, (pdf_obj *)Key, entry);
    if (code < 0)
        goto exit;

    code = pdfi_mark_setparam_obj(ctx, Value, entry+1);
    if (code < 0)
        goto exit;

 exit:
    return code;
}


/* Note: this isn't part of the obj_to_string() stuff */
static int pdfi_mark_ctm_str(pdf_context *ctx, gs_matrix *ctm, byte **data, int *len)
{
    int size = 100;
    char *buf;

    buf = (char *)gs_alloc_bytes(ctx->memory, size, "pdfi_mark_ctm_str(data)");
    if (buf == NULL)
        return_error(gs_error_VMerror);
    snprintf(buf, size, "[%.4f %.4f %.4f %.4f %.4f%.4f]",
             ctm->xx, ctm->xy, ctm->yx, ctm->yy, ctm->tx, ctm->ty);
    *data = (byte *)buf;
    *len = strlen(buf);
    return 0;
}

/* Write an string array command to the device (e.g. pdfmark) */
static int pdfi_mark_write_array(pdf_context *ctx, gs_param_string_array *array_list, const char *command)
{
    gs_c_param_list list;
    int code;

    /* Set the list to writeable, and initialise it */
    gs_c_param_list_write(&list, ctx->memory);
    /* We don't want keys to be persistent, as we are going to throw
     * away our array, force them to be copied
     */
    gs_param_list_set_persistent_keys((gs_param_list *) &list, false);

    /* Make really sure the list is writable, but don't initialise it */
    gs_c_param_list_write_more(&list);

    /* Add the param string array to the list */
    code = param_write_string_array((gs_param_list *)&list, command, array_list);
    if (code < 0)
        return code;

    /* Set the param list back to readable, so putceviceparams can readit (mad...) */
    gs_c_param_list_read(&list);

    /* and set the actual device parameters */
    code = gs_putdeviceparams(ctx->pgs->device, (gs_param_list *)&list);

    gs_c_param_list_release(&list);

    return code;
}

/* Write an string array command to the device (e.g. pdfmark) */
static int pdfi_mark_write_string(pdf_context *ctx, gs_param_string *param_string, const char *command)
{
    gs_c_param_list list;
    int code;

    /* Set the list to writeable, and initialise it */
    gs_c_param_list_write(&list, ctx->memory);
    /* We don't want keys to be persistent, as we are going to throw
     * away our array, force them to be copied
     */
    gs_param_list_set_persistent_keys((gs_param_list *) &list, false);

    /* Make really sure the list is writable, but don't initialise it */
    gs_c_param_list_write_more(&list);

    /* Add the param string array to the list */
    code = param_write_string((gs_param_list *)&list, command, param_string);
    if (code < 0)
        return code;

    /* Set the param list back to readable, so putceviceparams can readit (mad...) */
    gs_c_param_list_read(&list);

    /* and set the actual device parameters */
    code = gs_putdeviceparams(ctx->pgs->device, (gs_param_list *)&list);

    gs_c_param_list_release(&list);

    return code;
}

/* Apparently the strings to pass to the device are:
   key1 val1 .... keyN valN CTM "ANN"
   CTM is (for example) "[1.0 0 0 1.0 0 0]"

   the /pdfmark command has this array of strings as a parameter, i.e.
   [ key1 val1 .... keyN valN CTM "ANN" ] /pdfmark

   Apparently the "type" doesn't have a leading "/" but the other names in the
   keys do need the "/"

   See plparams.c/process_pdfmark()
*/
int pdfi_mark_from_dict(pdf_context *ctx, pdf_dict *dict, gs_matrix *ctm, const char *type)
{
    int code = 0;
    int size;
    uint64_t dictsize;
    uint64_t index;
    uint64_t keynum = 0;
    pdf_name *Key = NULL;
    pdf_obj *Value = NULL;
    gs_param_string *parray = NULL;
    gs_param_string_array array_list;
    byte *ctm_data = NULL;
    int ctm_len;
    gs_matrix ctm_placeholder;

    /* If ctm not provided, make a placeholder */
    if (!ctm) {
        gs_currentmatrix(ctx->pgs, &ctm_placeholder);
        ctm = &ctm_placeholder;
    }

    dictsize = pdfi_dict_entries(dict);
    size = dictsize*2 + 2; /* pairs + CTM + type */

    parray = (gs_param_string *)gs_alloc_bytes(ctx->memory, size*sizeof(gs_param_string),
                                               "pdfi_mark_from_dict(parray)");
    if (parray == NULL) {
        code = gs_note_error(gs_error_VMerror);
        goto exit;
    }
    memset(parray, 0, size *sizeof(gs_param_string));


    /* Get each (key,val) pair from dict and setup param for it */
    if (dictsize > 0) {
        code = pdfi_dict_key_first(ctx, dict, (pdf_obj **)&Key, &index);
        while (code >= 0) {
            code = pdfi_dict_get_by_key(ctx, dict, Key, &Value);
            if (code < 0) goto exit;
            code = pdfi_mark_setparam_pair(ctx, Key, Value, parray+(keynum*2));
            if (code < 0) goto exit;

            pdfi_countdown(Key);
            Key = NULL;
            pdfi_countdown(Value);
            Value = NULL;

            code = pdfi_dict_key_next(ctx, dict, (pdf_obj **)&Key, &index);
            if (code == gs_error_undefined) {
                code = 0;
                break;
            }
            keynum ++;
        }
    }
    if (code < 0) goto exit;

    /* CTM */
    code = pdfi_mark_ctm_str(ctx, ctm, &ctm_data, &ctm_len);
    if (code < 0) goto exit;
    parray[size-2].data = ctm_data;
    parray[size-2].size = ctm_len;

    /* Type (e.g. ANN, DOCINFO) */
    parray[size-1].data = (const byte *)type;
    parray[size-1].size = strlen(type);

    array_list.data = parray;
    array_list.persistent = false;
    array_list.size = size;

    code = pdfi_mark_write_array(ctx, &array_list, "pdfmark");

 exit:
    pdfi_countdown(Key);
    Key = NULL;
    pdfi_countdown(Value);
    Value = NULL;
    for (keynum = 0; keynum < dictsize; keynum ++) {
        gs_free_object(ctx->memory, (byte *)parray[keynum*2].data, "pdfi_mark_from_dict(parray)");
        gs_free_object(ctx->memory, (byte *)parray[keynum*2+1].data, "pdfi_mark_from_dict(parray)");
    }
    if (ctm_data)
        gs_free_object(ctx->memory, ctm_data, "pdfi_mark_from_dict(ctm_data)");
    gs_free_object(ctx->memory, parray, "pdfi_mark_from_dict(parray)");
    return code;
}

/* Send an arbitrary object as a string, with command 'label' */
int pdfi_mark_object(pdf_context *ctx, pdf_obj *object, const char *label)
{
    gs_param_string param_string;
    int code = 0;

    code = pdfi_loop_detector_mark(ctx);
    if (code < 0)
        goto exit;
    if (object->object_num != 0)
        code = pdfi_loop_detector_add_object(ctx, object->object_num);
    code = pdfi_resolve_indirect(ctx, object, true);
    (void)pdfi_loop_detector_cleartomark(ctx);
    if (code < 0)
        goto exit;

    code = pdfi_mark_setparam_obj(ctx, object, &param_string);
    if (code < 0)
        goto exit;

    code = pdfi_mark_write_string(ctx, &param_string, label);
    if (code < 0)
        goto exit;
 exit:
    return code;
}

/* Convert a Dest array to the hacky Page and View keys that pdfwrite expects
 * dest_array: [<page_ref> <view_info>]
 * page_ref: indirect ref to a page dict
 * view_info: see table 8.2 in PDF 1.7, for example "/Fit"
 *
 * Removes /Dest and inserts two key pairs: /Page N and /View <view_info>
 * N is the page number, which starts at 1, not 0.
 */
static int pdfi_mark_add_Page_View(pdf_context *ctx, pdf_dict *link_dict, pdf_array *dest_array)
{
    int code = 0;
    int i;
    uint64_t page_num;
    pdf_dict *page_dict = NULL;
    pdf_array *view_array = NULL;
    uint64_t array_size;
    pdf_obj *temp_obj = NULL;

    code = pdfi_array_get_type(ctx, dest_array, 0, PDF_DICT, (pdf_obj **)&page_dict);
    if (code < 0) goto exit;

    /* Find out which page number this is */
    code = pdfi_page_get_number(ctx, page_dict, &page_num);
    if (code < 0) goto exit;

    /* Add /Page key to the link_dict
     * Of course pdfwrite is numbering its pages starting at 1, because... of course :(
     */
    code = pdfi_dict_put_int(ctx, link_dict, "Page", page_num+1);

    /* Build an array for /View, out of the remainder of the Dest entry */
    array_size = pdfi_array_size(dest_array) - 1;
    code = pdfi_array_alloc(ctx, array_size, &view_array);
    if (code < 0) goto exit;
    pdfi_countup(view_array);
    for (i=0; i<array_size; i++) {
        code = pdfi_array_get(ctx, dest_array, i+1, &temp_obj);
        if (code < 0) goto exit;
        code = pdfi_array_put(ctx, view_array, i, temp_obj);
        if (code < 0) goto exit;

        pdfi_countdown(temp_obj);
        temp_obj = NULL;
    }
    /* Add /View key to the link_dict */
    code = pdfi_dict_put(ctx, link_dict, "View", (pdf_obj *)view_array);
    if (code < 0) goto exit;

 exit:
    pdfi_countdown(temp_obj);
    pdfi_countdown(page_dict);
    pdfi_countdown(view_array);
    return code;
}

/* Lookup a Dest string(or name) in the Names array and try to resolve it */
static int pdfi_mark_handle_dest_names(pdf_context *ctx, pdf_dict *link_dict,
                                         pdf_obj *dest, pdf_array *Names)
{
    int code = 0;
    int i;
    uint64_t array_size;
    pdf_obj *name = NULL;
    pdf_dict *D_dict = NULL;
    bool found = false;
    pdf_array *dest_array = NULL;

    array_size = pdfi_array_size(Names);

    /* Needs to be an even number - pairs of [name, obj] */
    if (array_size % 2 != 0) {
        /* TODO: flag an error? */
        /* Let's just ignore the last unpaired item for now */
        array_size -= 1;
    }

    for (i=0; i<array_size; i+=2) {
        code = pdfi_array_get(ctx, Names, i, (pdf_obj **)&name);
        if (code < 0) goto exit;
        /* Note: in current implementation, PDF_STRING and PDF_NAME have all the same
         * fields, but just in case that changes I treat them separately here.
         */
        if (name->type == PDF_STRING && dest->type == PDF_STRING) {
            if (!pdfi_string_cmp((pdf_string *)name, (pdf_string *)dest)) {
                found = true;
                break;
            }
        } else if (name->type == PDF_NAME && dest->type == PDF_NAME) {
            if (!pdfi_name_cmp((pdf_name *)name, (pdf_name *)dest)) {
                found = true;
                break;
            }
        }
        pdfi_countdown(name);
        name = NULL;
    }

    if (!found) {
        /* TODO: flag a warning? */
        code = 0;
        goto exit;
    }

    /* Next entry is supposed to be a dict */
    code = pdfi_array_get(ctx, Names, i+1, (pdf_obj **)&D_dict);
    if (code < 0) goto exit;
    if (D_dict->type != PDF_DICT) {
        /* TODO: flag a warning? */
        code = 0;
        goto exit;
    }

    /* Dict is supposed to contain key "D" with Dest array */
    code = pdfi_dict_knownget_type(ctx, D_dict, "D", PDF_ARRAY, (pdf_obj **)&dest_array);
    if (code <= 0) goto exit;

    /* Process the dest_array to replace with /Page /View */
    code = pdfi_mark_add_Page_View(ctx, link_dict, dest_array);
    if (code < 0) goto exit;

 exit:
    pdfi_countdown(name);
    pdfi_countdown(D_dict);
    pdfi_countdown(dest_array);
    return code;
}

/* Special handling for "Dest" in Links
 * Will replace /Dest with /Page /View in link_dict (for pdfwrite)
 */
int pdfi_mark_modDest(pdf_context *ctx, pdf_dict *link_dict)
{
    int code = 0;
    pdf_dict *Dests = NULL;
    pdf_obj *Dest = NULL;
    bool delete_Dest = true;
    pdf_array *dest_array = NULL;
    pdf_array *Names = NULL;
    pdf_dict *Names_dict = NULL;

    code = pdfi_dict_get(ctx, link_dict, "Dest", (pdf_obj **)&Dest);
    if (code < 0) goto exit;

    code = pdfi_dict_knownget_type(ctx, ctx->Root, "Dests", PDF_DICT, (pdf_obj **)&Dests);
    if (code < 0) goto exit;

    code = pdfi_dict_knownget_type(ctx, ctx->Root, "Names", PDF_DICT, (pdf_obj **)&Names_dict);
    if (code < 0) goto exit;

    switch (Dest->type) {
    case PDF_ARRAY:
        code = pdfi_mark_add_Page_View(ctx, link_dict, (pdf_array *)Dest);
        if (code < 0) goto exit;
        break;
    case PDF_NAME:
    case PDF_STRING:
        if (Dest->type == PDF_NAME && Dests != NULL) {
            /* Case where it's a name to look up in Contents(Root) /Dests */
            code = pdfi_dict_get_by_key(ctx, Dests, (const pdf_name *)Dest, (pdf_obj **)&dest_array);
            if (code == gs_error_undefined) {
                /* TODO: Not found, should flag a warning */
                code = 0;
                goto exit;
            }
            if (code < 0) goto exit;
            if (dest_array->type != PDF_ARRAY) {
                code = gs_note_error(gs_error_typecheck);
                goto exit;
            }
            code = pdfi_mark_add_Page_View(ctx, link_dict, dest_array);
            if (code < 0) goto exit;
        } else if (Names_dict != NULL) {
            /* Looking in Catalog(Root) for /Names<</Dests<</Names [name dict array]>>>> */
            code = pdfi_dict_knownget_type(ctx, Names_dict, "Dests", PDF_DICT, (pdf_obj **)&Dests);
            if (code < 0) goto exit;
            if (code == 0) {
                /* TODO: Not found -- not sure if there is another case here or not */
                goto exit;
            }

            code = pdfi_dict_knownget_type(ctx, Dests, "Names", PDF_ARRAY, (pdf_obj **)&Names);
            if (code < 0) goto exit;
            if (code == 0) {
                /* TODO: Not found -- not sure if there is another case here or not */
                goto exit;
            }
            code = pdfi_mark_handle_dest_names(ctx, link_dict, Dest, Names);
            if (code < 0) goto exit;
        } else {
            /* TODO: Ignore it -- flag a warning? */
        }
        break;
    default:
        /* TODO: Ignore it -- flag a warning? */
        break;
    }

 exit:
    if (delete_Dest) {
        /* Delete the Dest key */
        code = pdfi_dict_delete(ctx, link_dict, "Dest");
        if (code < 0) goto exit;
    }
    pdfi_countdown(Dest);
    pdfi_countdown(Dests);
    pdfi_countdown(Names);
    pdfi_countdown(Names_dict);
    pdfi_countdown(dest_array);
    return code;
}

/* Special handling for "A" in Link annotations and Outlines
 * Will delete A if handled and if A_key is provided.
 */
int pdfi_mark_modA(pdf_context *ctx, pdf_dict *dict, bool *resolve)
{
    int code = 0;
    pdf_dict *A_dict = NULL;
    bool known;
    pdf_name *S_name = NULL;
    pdf_array *D_array = NULL;
    bool delete_A = false;

    *resolve = false;

    code = pdfi_dict_get(ctx, dict, "A", (pdf_obj **)&A_dict);
    if (code < 0) goto exit;

    if (A_dict->type != PDF_DICT) {
        /* Invalid AP, just delete it because I dunno what to do...
         * TODO: Should flag a warning here
         */
        delete_A = true;
        goto exit;
    }

    /* Handle URI */
    code = pdfi_dict_known(ctx, A_dict, "URI", &known);
    if (code < 0) goto exit;
    if (known) {
        *resolve = true;
        goto exit;
    }

    /* Handle S = GoTo */
    /* TODO: this handles <</S /GoTo /D [dest array]>>
     * Not sure if there are other cases to handle?
     */
    code = pdfi_dict_knownget_type(ctx, A_dict, "S", PDF_NAME, (pdf_obj **)&S_name);
    if (code <= 0) goto exit;
    /* We only handle GoTo for now */
    if (pdfi_name_is(S_name, "GoTo")) {
        code = pdfi_dict_knownget_type(ctx, A_dict, "D", PDF_ARRAY, (pdf_obj **)&D_array);
        if (code == 0) goto exit;
        if (code < 0) {
            if (code == gs_error_typecheck) {
                /* TODO: Are there other cases to handle?
                 * Sample tests_private/pdf/sumatra/recursive_action_destinations.pdf
                 * has a recursive destination that has an indirect ref here.  We return a
                 * typecheck and that causes us to omit the whole thing, but is that
                 * really the best treatment?
                 */
            }
            goto exit;
        }
        /* Process the D array to replace with /Page /View */
        code = pdfi_mark_add_Page_View(ctx, dict, D_array);
        if (code < 0) goto exit;
        delete_A = true;
    } else if (pdfi_name_is(S_name, "GoToR") || pdfi_name_is(S_name, "Launch")) {
        /* TODO: I think we just leave this alone since it points out of doc?
         * Currently this is same behavior as gs, but it is not correct behavior.
         * In at least some cases we could do better, for example if the doc
         * pointed to happens to be the same file.
         * Sample: fts_28_2808.pdf
         */
        delete_A = false;
        *resolve = true;
        code = 0;
    } else if (pdfi_name_is(S_name, "Named")) {
        /* We can just pass this through and it will work fine
         * This should be a name like "FirstPage" or "LastPage".
         * Note: gs implementation translates into page numbers and also has some bugs...
         * Sample: fts_33_3310.pdf
         */
        delete_A = false;
        code = 0;
    } else if (pdfi_name_is(S_name, "GoToE")) {
        /* TODO: ??
         * File: fts_33_3303.pdf
         */
    } else if (pdfi_name_is(S_name, "Thread")) {
        /* TODO: For basically all of these below, I think just need to preserve
         * any references and streams and pass it all through.
         * File: fts_33_3305.pdf fts_33_3317.pdf
         */
    } else if (pdfi_name_is(S_name, "Sound")) {
        /* TODO: ??
         * File: fts_33_3307.pdf
         */
    } else if (pdfi_name_is(S_name, "Movie")) {
        /* TODO: ??
         * File: fts_33_3308.pdf
         */
    } else if (pdfi_name_is(S_name, "GoTo3DView")) {
        /* TODO: ??
         * File: fts_33_3318.pdf
         */
    } else if (pdfi_name_is(S_name, "RichMediaExecute")) {
        /* TODO: ??
         * File: fts_33_3319.pdf
         */
    } else if (pdfi_name_is(S_name, "Rendition")) {
        /* TODO: make sure to pass through accurately?
         * File: fts_07_0709.pdf fts_33_3316.pdf
         */
    } else {
        /* TODO: flag warning? */
    }

 exit:
    if (delete_A) {
        code = pdfi_dict_delete(ctx, dict, "A");
    }
    pdfi_countdown(A_dict);
    pdfi_countdown(S_name);
    pdfi_countdown(D_array);
    return code;
}