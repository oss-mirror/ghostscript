/* Copyright (C) 2001-2018 Artifex Software, Inc.
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

/* The PDF interpreter written in C */

#include "plmain.h"
#include "pdf_int.h"
#include "pdf_file.h"
#include "pdf_loop_detect.h"
#include "strmio.h"
#include "stream.h"
#include "pdf_path.h"
#include "pdf_colour.h"
#include "pdf_image.h"
#include "pdf_shading.h"
#include "pdf_font.h"
#include "pdf_text.h"
#include "pdf_gstate.h"
#include "pdf_stack.h"
#include "pdf_xref.h"
#include "pdf_dict.h"
#include "pdf_array.h"


/***********************************************************************************/
/* Functions to create the various kinds of 'PDF objects', Created objects have a  */
/* reference count of 0. Composite objects (dictionaries, arrays, strings) use the */
/* 'size' argument to create an object with the correct numbr of entries or of the */
/* requested size. Simple objects (integers etc) ignore this parameter.            */
/* Objects do not get their data assigned, that's up to the caller, but we do      */
/* set the lngth or size fields for composite objects.                             */

int pdfi_alloc_object(pdf_context *ctx, pdf_obj_type type, unsigned int size, pdf_obj **obj)
{
    int bytes = 0;

    switch(type) {
        case PDF_ARRAY_MARK:
        case PDF_DICT_MARK:
        case PDF_PROC_MARK:
        case PDF_NULL:
            bytes = sizeof(pdf_obj);
            break;
        case PDF_INT:
        case PDF_REAL:
            bytes = sizeof(pdf_num);
            break;
        case PDF_STRING:
        case PDF_NAME:
            bytes = sizeof(pdf_string);
            break;
        case PDF_ARRAY:
            bytes = sizeof(pdf_array);
            break;
        case PDF_DICT:
            bytes = sizeof(pdf_dict);
            break;
        case PDF_INDIRECT:
            bytes = sizeof(pdf_indirect_ref);
            break;
        case PDF_BOOL:
            bytes = sizeof(pdf_bool);
            break;
        case PDF_KEYWORD:
            bytes = sizeof(pdf_keyword);
            break;
        /* The following aren't PDF object types, but are objects we either want to
         * reference count, or store on the stack.
         */
        case PDF_XREF_TABLE:
            bytes = sizeof(xref_table);
            break;
        default:
            return_error(gs_error_typecheck);
    }
    *obj = (pdf_obj *)gs_alloc_bytes(ctx->memory, bytes, "pdfi_alloc_object");
    if (*obj == NULL)
        return_error(gs_error_VMerror);

    memset(*obj, 0x00, bytes);
    (*obj)->memory = ctx->memory;
    (*obj)->type = type;

    switch(type) {
        case PDF_NULL:
        case PDF_INT:
        case PDF_REAL:
        case PDF_INDIRECT:
        case PDF_BOOL:
        case PDF_ARRAY_MARK:
        case PDF_DICT_MARK:
        case PDF_PROC_MARK:
            break;
        case PDF_KEYWORD:
        case PDF_STRING:
        case PDF_NAME:
            {
                unsigned char *data = NULL;
                data = (unsigned char *)gs_alloc_bytes(ctx->memory, size, "pdfi_alloc_object");
                if (data == NULL) {
                    gs_free_object(ctx->memory, *obj, "pdfi_alloc_object");
                    *obj = NULL;
                    return_error(gs_error_VMerror);
                }
                ((pdf_string *)*obj)->data = data;
                ((pdf_string *)*obj)->length = size;
            }
            break;
        case PDF_ARRAY:
            {
                pdf_obj **values = NULL;

                ((pdf_array *)*obj)->size = size;
                if (size > 0) {
                    values = (pdf_obj **)gs_alloc_bytes(ctx->memory, size * sizeof(pdf_obj *), "pdfi_alloc_object");
                    if (values == NULL) {
                        gs_free_object(ctx->memory, *obj, "pdfi_alloc_object");
                        gs_free_object(ctx->memory, values, "pdfi_alloc_object");
                        *obj = NULL;
                        return_error(gs_error_VMerror);
                    }
                    ((pdf_array *)*obj)->values = values;
                    memset(((pdf_array *)*obj)->values, 0x00, size * sizeof(pdf_obj *));
                }
            }
            break;
        case PDF_DICT:
            {
                pdf_obj **keys = NULL, **values = NULL;

                ((pdf_dict *)*obj)->size = size;
                if (size > 0) {
                    keys = (pdf_obj **)gs_alloc_bytes(ctx->memory, size * sizeof(pdf_obj *), "pdfi_alloc_object");
                    values = (pdf_obj **)gs_alloc_bytes(ctx->memory, size * sizeof(pdf_obj *), "pdfi_alloc_object");
                    if (keys == NULL || values == NULL) {
                        gs_free_object(ctx->memory, *obj, "pdfi_alloc_object");
                        gs_free_object(ctx->memory, keys, "pdfi_alloc_object");
                        gs_free_object(ctx->memory, values, "pdfi_alloc_object");
                        *obj = NULL;
                        return_error(gs_error_VMerror);
                    }
                    ((pdf_dict *)*obj)->values = values;
                    ((pdf_dict *)*obj)->keys = keys;
                    memset(((pdf_dict *)*obj)->values, 0x00, size * sizeof(pdf_obj *));
                    memset(((pdf_dict *)*obj)->keys, 0x00, size * sizeof(pdf_obj *));
                }
            }
            break;
        /* The following aren't PDF object types, but are objects we either want to
         * reference count, or store on the stack.
         */
        case PDF_XREF_TABLE:
            break;
        default:
            break;
    }
#if REFCNT_DEBUG
    (*obj)->ctx = (void *)ctx;
    (*obj)->UID = ctx->UID++;
    dmprintf2(ctx->memory, "Allocated object of type %c with UID %"PRIi64"\n", (*obj)->type, (*obj)->UID);
#endif
    return 0;
}

/***********************************************************************************/
/* Functions to free the various kinds of 'PDF objects' and stack manipulations.   */
/* All objects are reference counted. Pushign an object onto the stack increments  */
/* its reference count, popping it from the stack decrements its reference count.  */
/* When an object's reference count is decremented to 0, the relevant 'free'       */
/* function is called to free the object.                                          */

static void pdfi_free_namestring(pdf_obj *o)
{
    /* Currently names and strings are the same, so a single cast is OK */
    pdf_name *n = (pdf_name *)o;

    if (n->data != NULL)
        gs_free_object(n->memory, n->data, "pdf interpreter free name or string data");
    gs_free_object(n->memory, n, "pdf interpreter free name or string");
}

static void pdfi_free_keyword(pdf_obj *o)
{
    /* Currently names and strings are the same, so a single cast is OK */
    pdf_keyword *k = (pdf_keyword *)o;

    if (k->data != NULL)
        gs_free_object(k->memory, k->data, "pdf interpreter free keyword data");
    gs_free_object(k->memory, k, "pdf interpreter free keyword");
}

static void pdfi_free_xref_table(pdf_obj *o)
{
    xref_table *xref = (xref_table *)o;

    gs_free_object(xref->memory, xref->xref, "pdfi_free_xref_table");
    gs_free_object(xref->memory, xref, "pdfi_free_xref_table");
}

void pdfi_free_object(pdf_obj *o)
{
    switch(o->type) {
        case PDF_ARRAY_MARK:
        case PDF_DICT_MARK:
        case PDF_NULL:
        case PDF_INT:
        case PDF_REAL:
        case PDF_INDIRECT:
        case PDF_BOOL:
            gs_free_object(o->memory, o, "pdf interpreter object refcount to 0");
            break;
        case PDF_STRING:
        case PDF_NAME:
            pdfi_free_namestring(o);
            break;
        case PDF_ARRAY:
            pdfi_free_array(o);
            break;
        case PDF_DICT:
            pdfi_free_dict(o);
            break;
        case PDF_KEYWORD:
            pdfi_free_keyword(o);
            break;
        case PDF_XREF_TABLE:
            pdfi_free_xref_table(o);
            break;
        default:
            dbgmprintf(o->memory, "!!! Attempting to free unknown obect type !!!\n");
            break;
    }
}

/***********************************************************************************/
/* Utility Functions                                                               */
int
pdfi_name_strcmp(const pdf_name *n, const char *s)
{
    int len = strlen(s);
    if (n->length == len)
        return memcmp(n->data, s, len);
    return -1;
}

int
pdfi_name_cmp(const pdf_name *n1, const pdf_name *n2)
{
    if (n1->length != n2->length)
        return -1;
    return memcmp(n1->data, n2->data, n1->length);
}


/***********************************************************************************/
/* Functions to dereference object references and manage the object cache          */

static int pdfi_add_to_cache(pdf_context *ctx, pdf_obj *o)
{
    pdf_obj_cache_entry *entry;

    if (o->object_num > ctx->xref_table->xref_size)
        return_error(gs_error_rangecheck);

    if (ctx->cache_entries == MAX_OBJECT_CACHE_SIZE)
    {
        dbgmprintf(ctx->memory, "Cache full, evicting LRU\n");
        if (ctx->cache_LRU) {
            entry = ctx->cache_LRU;
            ctx->cache_LRU = entry->next;
            if (entry->next)
                ((pdf_obj_cache_entry *)entry->next)->previous = NULL;
            ctx->xref_table->xref[entry->o->object_num].cache = NULL;
            pdfi_countdown(entry->o);
            ctx->cache_entries--;
            gs_free_object(ctx->memory, entry, "pdfi_add_to_cache, free LRU");
        } else
            return_error(gs_error_unknownerror);
    }
    entry = (pdf_obj_cache_entry *)gs_alloc_bytes(ctx->memory, sizeof(pdf_obj_cache_entry), "pdfi_add_to_cache");
    if (entry == NULL)
        return_error(gs_error_VMerror);

    memset(entry, 0x00, sizeof(pdf_obj_cache_entry));

    entry->o = o;
    pdfi_countup(o);
    if (ctx->cache_MRU) {
        entry->previous = ctx->cache_MRU;
        ctx->cache_MRU->next = entry;
    }
    ctx->cache_MRU = entry;
    if (ctx->cache_LRU == NULL)
        ctx->cache_LRU = entry;

    ctx->cache_entries++;
    ctx->xref_table->xref[o->object_num].cache = entry;
    return 0;
}

/* pdf_dereference returns an object with a reference count of at least 1, this represents the
 * reference being held by the caller (in **object) when we return from this function.
 */
int pdfi_dereference(pdf_context *ctx, uint64_t obj, uint64_t gen, pdf_obj **object)
{
    xref_entry *entry;
    pdf_obj *o;
    int code;
    gs_offset_t saved_stream_offset;

    if (obj >= ctx->xref_table->xref_size) {
        dmprintf1(ctx->memory, "Error, attempted to dereference object %"PRIu64", which is not present in the xref table\n", obj);
        ctx->pdf_errors |= E_PDF_BADOBJNUMBER;

        if(ctx->pdfstoponerror)
            return_error(gs_error_rangecheck);

        code = pdfi_alloc_object(ctx, PDF_NULL, 0, object);
        if (code == 0)
            pdfi_countup(*object);
        return code;
    }

    entry = &ctx->xref_table->xref[obj];

    if(entry->object_num == 0)
        return_error(gs_error_undefined);

    if (entry->free) {
        dmprintf1(ctx->memory, "Attempt to dereference free object %"PRIu64", returning a null object.\n", entry->object_num);
        code = pdfi_alloc_object(ctx, PDF_NULL, 1, object);
        if (code > 0)
            pdfi_countup(*object);
        return code;
    }

    if (ctx->loop_detection) {
        if (pdfi_loop_detector_check_object(ctx, obj) == true)
            return_error(gs_error_circular_reference);
    }
    if (entry->cache != NULL){
        pdf_obj_cache_entry *cache_entry = entry->cache;

        *object = cache_entry->o;
        pdfi_countup(*object);
        if (ctx->cache_MRU && cache_entry != ctx->cache_MRU) {
            if ((pdf_obj_cache_entry *)cache_entry->next != NULL)
                ((pdf_obj_cache_entry *)cache_entry->next)->previous = cache_entry->previous;
            if ((pdf_obj_cache_entry *)cache_entry->previous != NULL)
                ((pdf_obj_cache_entry *)cache_entry->previous)->next = cache_entry->next;
            else {
                /* the existing entry is the current least recently used, we need to make the 'next'
                 * cache entry into the LRU.
                 */
                ctx->cache_LRU = cache_entry->next;
            }
            cache_entry->next = NULL;
            cache_entry->previous = ctx->cache_MRU;
            ctx->cache_MRU->next = cache_entry;
            ctx->cache_MRU = cache_entry;
        }
    } else {
        saved_stream_offset = pdfi_unread_tell(ctx);

        if (entry->compressed) {
            /* This is an object in a compressed object stream */

            xref_entry *compressed_entry = &ctx->xref_table->xref[entry->u.compressed.compressed_stream_num];
            pdf_dict *compressed_object;
            pdf_stream *compressed_stream;
            char Buffer[256];
            int i = 0;
            int64_t num_entries;
            gs_offset_t offset = 0;

            if (ctx->pdfdebug) {
                dmprintf1(ctx->memory, "%% Reading compressed object %"PRIi64, obj);
                dmprintf1(ctx->memory, " from ObjStm with object number %"PRIi64"\n", compressed_entry->object_num);
            }

            if (compressed_entry->cache == NULL) {
                code = pdfi_seek(ctx, ctx->main_stream, compressed_entry->u.uncompressed.offset, SEEK_SET);
                if (code < 0) {
                    (void)pdfi_seek(ctx, ctx->main_stream, saved_stream_offset, SEEK_SET);
                    return code;
                }

                code = pdfi_read_object_of_type(ctx, ctx->main_stream, PDF_DICT);
                if (code < 0){
                    (void)pdfi_seek(ctx, ctx->main_stream, saved_stream_offset, SEEK_SET);
                    return code;
                }

                compressed_object = (pdf_dict *)ctx->stack_top[-1];
                pdfi_countup(compressed_object);
                pdfi_pop(ctx, 1);
            } else {
                compressed_object = (pdf_dict *)compressed_entry->cache->o;
                pdfi_countup(compressed_object);
            }
            /* Check its an ObjStm ! */
            code = pdfi_dict_get_type(ctx, compressed_object, "Type", PDF_NAME, &o);
            if (code < 0) {
                pdfi_countdown(compressed_object);
                (void)pdfi_seek(ctx, ctx->main_stream, saved_stream_offset, SEEK_SET);
                return code;
            }
            if (((pdf_name *)o)->length != 6 || memcmp(((pdf_name *)o)->data, "ObjStm", 6) != 0){
                pdfi_countdown(o);
                pdfi_countdown(compressed_object);
                (void)pdfi_seek(ctx, ctx->main_stream, saved_stream_offset, SEEK_SET);
                return_error(gs_error_syntaxerror);
            }
            pdfi_countdown(o);

            /* Need to check the /N entry to see if the object is actually in this stream! */
            code = pdfi_dict_get_int(ctx, compressed_object, "N", &num_entries);
            if (code < 0) {
                pdfi_countdown(compressed_object);
                (void)pdfi_seek(ctx, ctx->main_stream, saved_stream_offset, SEEK_SET);
                return code;
            }
            if (num_entries < 0 || num_entries > ctx->xref_table->xref_size) {
                pdfi_countdown(compressed_object);
                (void)pdfi_seek(ctx, ctx->main_stream, saved_stream_offset, SEEK_SET);
                return_error(gs_error_rangecheck);
            }

            code = pdfi_seek(ctx, ctx->main_stream, compressed_object->stream_offset, SEEK_SET);
            if (code < 0) {
                pdfi_countdown(compressed_object);
                (void)pdfi_seek(ctx, ctx->main_stream, saved_stream_offset, SEEK_SET);
                return code;
            }

            code = pdfi_filter(ctx, compressed_object, ctx->main_stream, &compressed_stream, false);
            if (code < 0) {
                pdfi_countdown(compressed_object);
                (void)pdfi_seek(ctx, ctx->main_stream, saved_stream_offset, SEEK_SET);
                return code;
            }

            for (i=0;i < num_entries;i++)
            {
                code = pdfi_read_token(ctx, compressed_stream);
                if (code < 0) {
                    pdfi_close_file(ctx, compressed_stream);
                    pdfi_countdown(compressed_object);
                    (void)pdfi_seek(ctx, ctx->main_stream, saved_stream_offset, SEEK_SET);
                    return code;
                }
                o = ctx->stack_top[-1];
                if (((pdf_obj *)o)->type != PDF_INT) {
                    pdfi_pop(ctx, 1);
                    pdfi_close_file(ctx, compressed_stream);
                    pdfi_countdown(compressed_object);
                    (void)pdfi_seek(ctx, ctx->main_stream, saved_stream_offset, SEEK_SET);
                    return_error(gs_error_typecheck);
                }
                pdfi_pop(ctx, 1);
                code = pdfi_read_token(ctx, compressed_stream);
                if (code < 0) {
                    pdfi_close_file(ctx, compressed_stream);
                    pdfi_countdown(compressed_object);
                    (void)pdfi_seek(ctx, ctx->main_stream, saved_stream_offset, SEEK_SET);
                    return code;
                }
                o = ctx->stack_top[-1];
                if (((pdf_obj *)o)->type != PDF_INT) {
                    pdfi_pop(ctx, 1);
                    pdfi_close_file(ctx, compressed_stream);
                    pdfi_countdown(compressed_object);
                    (void)pdfi_seek(ctx, ctx->main_stream, saved_stream_offset, SEEK_SET);
                    return_error(gs_error_typecheck);
                }
                if (i == entry->u.compressed.object_index)
                    offset = ((pdf_num *)o)->value.i;
                pdfi_pop(ctx, 1);
            }

            for (i=0;i < offset;i++)
            {
                code = pdfi_read_bytes(ctx, (byte *)&Buffer[0], 1, 1, compressed_stream);
                if (code <= 0) {
                    pdfi_close_file(ctx, compressed_stream);
                    return_error(gs_error_ioerror);
                }
            }

            code = pdfi_read_token(ctx, compressed_stream);
            if (code < 0) {
                pdfi_close_file(ctx, compressed_stream);
                pdfi_countdown(compressed_object);
                (void)pdfi_seek(ctx, ctx->main_stream, saved_stream_offset, SEEK_SET);
                return code;
            }
            if (ctx->stack_top[-1]->type == PDF_ARRAY_MARK || ctx->stack_top[-1]->type == PDF_DICT_MARK) {
                int start_depth = ctx->stack_top - ctx->stack_bot;

                /* Need to read all the elements from COS objects */
                do {
                    code = pdfi_read_token(ctx, compressed_stream);
                    if (code < 0) {
                        pdfi_close_file(ctx, compressed_stream);
                        pdfi_countdown(compressed_object);
                        (void)pdfi_seek(ctx, ctx->main_stream, saved_stream_offset, SEEK_SET);
                        return code;
                    }
                    if (compressed_stream->eof == true) {
                        pdfi_close_file(ctx, compressed_stream);
                        pdfi_countdown(compressed_object);
                        (void)pdfi_seek(ctx, ctx->main_stream, saved_stream_offset, SEEK_SET);
                        return_error(gs_error_ioerror);
                    }
                }while ((ctx->stack_top[-1]->type != PDF_ARRAY && ctx->stack_top[-1]->type != PDF_DICT) || ctx->stack_top - ctx->stack_bot > start_depth);
            }

            pdfi_close_file(ctx, compressed_stream);

            *object = ctx->stack_top[-1];
            /* For compressed objects we don't get a 'obj gen obj' sequence which is what sets
             * the object number for uncompressed objects. So we need to do that here.
             */
            (*object)->object_num = obj;
            (*object)->generation_num = gen;
            pdfi_countup(*object);
            pdfi_pop(ctx, 1);

            pdfi_countdown(compressed_object);
        } else {
            code = pdfi_seek(ctx, ctx->main_stream, entry->u.uncompressed.offset, SEEK_SET);
            if (code < 0) {
                (void)pdfi_seek(ctx, ctx->main_stream, saved_stream_offset, SEEK_SET);
                return code;
            }

            code = pdfi_read_object(ctx, ctx->main_stream);
            if (code < 0) {
                (void)pdfi_seek(ctx, ctx->main_stream, saved_stream_offset, SEEK_SET);
                return code;
            }

            *object = ctx->stack_top[-1];
            pdfi_pop(ctx, 1);
            if ((*object)->object_num == obj)
                pdfi_countup(*object);
            else
                return_error(gs_error_undefined);
        }
        (void)pdfi_seek(ctx, ctx->main_stream, saved_stream_offset, SEEK_SET);
    }

    if (ctx->loop_detection) {
        code = pdfi_loop_detector_add_object(ctx, (*object)->object_num);
        if (code < 0)
            return code;
    }
    return 0;
}

void normalize_rectangle(double *d)
{
    double d1[4];
    int i;

    if (d[0] < d[2]) {
        d1[0] = d[0];
        d1[2] = d[2];
    } else {
        d1[0] = d[2];
        d1[2] = d[0];
    }
    if (d[1] < d[3]) {
        d1[1] = d[1];
        d1[3] = d[3];
    } else {
        d1[1] = d[3];
        d1[3] = d[1];
    }
    for (i=0;i<=3;i++){
        d[i] = d1[i];
    }
}

/***********************************************************************************/
/* 'token' reading functions. Tokens in this sense are PDF logical objects and the */
/* related keywords. So that's numbers, booleans, names, strings, dictionaries,    */
/* arrays, the  null object and indirect references. The keywords are obj/endobj   */
/* stream/endstream, xref, startxref and trailer.                                  */

/***********************************************************************************/
/* Some simple functions to find white space, delimiters and hex bytes             */
static bool iswhite(char c)
{
    if (c == 0x00 || c == 0x09 || c == 0x0a || c == 0x0c || c == 0x0d || c == 0x20)
        return true;
    else
        return false;
}

static bool isdelimiter(char c)
{
    if (c == '/' || c == '(' || c == ')' || c == '[' || c == ']' || c == '<' || c == '>' || c == '{' || c == '}' || c == '%')
        return true;
    else
        return false;
}

static bool ishex(char c)
{
    if (c < 0x30)
        return false;

    if (c > 0x39) {
        if (c > 'F') {
            if (c < 'a')
                return false;
            if (c > 'f')
                return false;
            return true;
        } else {
            if (c < 'A')
                return false;
            return true;
        }
    } else
        return true;
}

/* You must ensure the character is a hex character before calling this, no error trapping here */
static int fromhex(char c)
{
    if (c > 0x39) {
        if (c > 'F') {
            return c - 0x57;
        } else {
            return c - 0x37;
        }
    } else
        return c - 0x30;
}

/* The 'read' functions all return the newly created object on the context's stack
 * which means these objects are created with a reference count of 0, and only when
 * pushed onto the stack does the reference count become 1, indicating the stack is
 * the only reference.
 */
int skip_white(pdf_context *ctx, pdf_stream *s)
{
    uint32_t read = 0;
    int32_t bytes = 0;
    byte c;

    do {
        bytes = pdfi_read_bytes(ctx, &c, 1, 1, s);
        if (bytes < 0)
            return_error(gs_error_ioerror);
        if (bytes == 0)
            return 0;
        read += bytes;
    } while (bytes != 0 && iswhite(c));

    if (read > 0)
        pdfi_unread(ctx, s, &c, 1);
    return 0;
}

static int skip_eol(pdf_context *ctx, pdf_stream *s)
{
    uint32_t read = 0;
    int32_t bytes = 0;
    byte c;

    do {
        bytes = pdfi_read_bytes(ctx, &c, 1, 1, s);
        if (bytes < 0)
            return_error(gs_error_ioerror);
        if (bytes == 0)
            return 0;
        read += bytes;
    } while (bytes != 0 && (c == 0x0A || c == 0x0D));

    if (read > 0)
        pdfi_unread(ctx, s, &c, 1);
    return 0;
}

static int pdfi_read_num(pdf_context *ctx, pdf_stream *s)
{
    byte Buffer[256];
    unsigned short index = 0;
    short bytes;
    bool real = false;
    pdf_num *num;
    int code = 0, malformed = false;

    skip_white(ctx, s);

    do {
        bytes = pdfi_read_bytes(ctx, (byte *)&Buffer[index], 1, 1, s);
        if (bytes == 0 && s->eof) {
            Buffer[index] = 0x00;
            break;
        }

        if (bytes <= 0)
            return_error(gs_error_ioerror);

        if (iswhite((char)Buffer[index])) {
            Buffer[index] = 0x00;
            break;
        } else {
            if (isdelimiter((char)Buffer[index])) {
                pdfi_unread(ctx, s, (byte *)&Buffer[index], 1);
                Buffer[index] = 0x00;
                break;
            }
        }
        if (Buffer[index] == '.')
            if (real == true) {
                if (ctx->pdfstoponerror)
                    return_error(gs_error_syntaxerror);
                malformed = true;
            } else
                real = true;
        else {
            if (Buffer[index] == '-' || Buffer[index] == '+') {
                if (index != 0) {
                    if (ctx->pdfstoponerror)
                        return_error(gs_error_syntaxerror);
                    malformed = true;
                }
            } else {
                if (Buffer[index] < 0x30 || Buffer[index] > 0x39) {
                    if (ctx->pdfstoponerror)
                        return_error(gs_error_syntaxerror);
                    if (!(ctx->pdf_errors & E_PDF_MISSINGWHITESPACE))
                        dmprintf(ctx->memory, "Ignoring missing white space while parsing number\n");
                    ctx->pdf_errors |= E_PDF_MISSINGWHITESPACE;
                    pdfi_unread(ctx, s, (byte *)&Buffer[index], 1);
                    Buffer[index] = 0x00;
                    break;
                }
            }
        }
        if (++index > 255)
            return_error(gs_error_syntaxerror);
    } while(1);

    if (real && !malformed)
        code = pdfi_alloc_object(ctx, PDF_REAL, 0, (pdf_obj **)&num);
    else
        code = pdfi_alloc_object(ctx, PDF_INT, 0, (pdf_obj **)&num);
    if (code < 0)
        return code;

    if (malformed) {
        if (!(ctx->pdf_errors & E_PDF_MALFORMEDNUMBER))
            dmprintf1(ctx->memory, "Treating malformed number %s as 0.\n", Buffer);
        ctx->pdf_errors |= E_PDF_MALFORMEDNUMBER;
        num->value.i = 0;
    } else {
        if (real) {
            float tempf;
            if (sscanf((const char *)Buffer, "%f", &tempf) == 0) {
                if (ctx->pdfdebug)
                    dmprintf1(ctx->memory, "failed to read real number : %s\n", Buffer);
                gs_free_object(num->memory, num, "pdfi_read_num error");
                return_error(gs_error_syntaxerror);
            }
            num->value.d = tempf;
        } else {
            int tempi;
            if (sscanf((const char *)Buffer, "%d", &tempi) == 0) {
                if (ctx->pdfdebug)
                    dmprintf1(ctx->memory, "failed to read integer : %s\n", Buffer);
                gs_free_object(num->memory, num, "pdfi_read_num error");
                return_error(gs_error_syntaxerror);
            }
            num->value.i = tempi;
        }
    }
    if (ctx->pdfdebug) {
        if (real)
            dmprintf1(ctx->memory, " %f", num->value.d);
        else
            dmprintf1(ctx->memory, " %"PRIi64, num->value.i);
    }

    code = pdfi_push(ctx, (pdf_obj *)num);

    if (code < 0)
        pdfi_free_object((pdf_obj *)num);

    return code;
}

static int pdfi_read_name(pdf_context *ctx, pdf_stream *s)
{
    char *Buffer, *NewBuf = NULL;
    unsigned short index = 0;
    short bytes = 0;
    uint32_t size = 256;
    pdf_name *name = NULL;
    int code;

    Buffer = (char *)gs_alloc_bytes(ctx->memory, size, "pdfi_read_name");
    if (Buffer == NULL)
        return_error(gs_error_VMerror);

    do {
        bytes = pdfi_read_bytes(ctx, (byte *)&Buffer[index], 1, 1, s);
        if (bytes == 0 && s->eof)
            break;
        if (bytes <= 0)
            return_error(gs_error_ioerror);

        if (iswhite((char)Buffer[index])) {
            Buffer[index] = 0x00;
            break;
        } else {
            if (isdelimiter((char)Buffer[index])) {
                pdfi_unread(ctx, s, (byte *)&Buffer[index], 1);
                Buffer[index] = 0x00;
                break;
            }
        }

        /* Check for and convert escaped name characters */
        if (Buffer[index] == '#') {
            byte NumBuf[2];

            bytes = pdfi_read_bytes(ctx, (byte *)&NumBuf, 1, 2, s);
            if (bytes < 2) {
                gs_free_object(ctx->memory, Buffer, "pdfi_read_name error");
                return_error(gs_error_ioerror);
            }

            if (!ishex(NumBuf[0]) || !ishex(NumBuf[1])) {
                gs_free_object(ctx->memory, Buffer, "pdfi_read_name error");
                return_error(gs_error_ioerror);
            }

            Buffer[index] = (fromhex(NumBuf[0]) << 4) + fromhex(NumBuf[1]);
        }

        /* If we ran out of memory, increase the buffer size */
        if (index++ >= size - 1) {
            NewBuf = (char *)gs_alloc_bytes(ctx->memory, size + 256, "pdfi_read_name");
            if (NewBuf == NULL) {
                gs_free_object(ctx->memory, Buffer, "pdfi_read_name error");
                return_error(gs_error_VMerror);
            }
            memcpy(NewBuf, Buffer, size);
            gs_free_object(ctx->memory, Buffer, "pdfi_read_name");
            Buffer = NewBuf;
            size += 256;
        }
    } while(1);

    code = pdfi_alloc_object(ctx, PDF_NAME, index, (pdf_obj **)&name);
    if (code < 0) {
        gs_free_object(ctx->memory, Buffer, "pdfi_read_name error");
        return code;
    }
    memcpy(name->data, Buffer, index);

    if (ctx->pdfdebug)
        dmprintf1(ctx->memory, " /%s", Buffer);

    gs_free_object(ctx->memory, Buffer, "pdfi_read_name");

    code = pdfi_push(ctx, (pdf_obj *)name);

    if (code < 0)
        pdfi_free_namestring((pdf_obj *)name);

    return code;
}

static int pdfi_read_hexstring(pdf_context *ctx, pdf_stream *s)
{
    char *Buffer, *NewBuf = NULL, HexBuf[2];
    unsigned short index = 0;
    short bytes = 0;
    uint32_t size = 256;
    pdf_string *string = NULL;
    int code;

    Buffer = (char *)gs_alloc_bytes(ctx->memory, size, "pdfi_read_hexstring");
    if (Buffer == NULL)
        return_error(gs_error_VMerror);

    if (ctx->pdfdebug)
        dmprintf(ctx->memory, " <");

    do {
        do {
            bytes = pdfi_read_bytes(ctx, (byte *)HexBuf, 1, 1, s);
            if (bytes == 0 && s->eof)
                break;
            if (bytes <= 0)
                return_error(gs_error_ioerror);
        } while(iswhite(HexBuf[0]));
        if (bytes == 0 && s->eof)
            break;

        if (HexBuf[0] == '>')
            break;

        if (ctx->pdfdebug)
            dmprintf1(ctx->memory, "%c", HexBuf[0]);

        do {
            bytes = pdfi_read_bytes(ctx, (byte *)&HexBuf[1], 1, 1, s);
            if (bytes == 0 && s->eof)
                break;
            if (bytes <= 0)
                return_error(gs_error_ioerror);
        } while(iswhite(HexBuf[1]));
        if (bytes == 0 && s->eof)
            break;

        if (!ishex(HexBuf[0]) || !ishex(HexBuf[1]))
            return_error(gs_error_syntaxerror);

        if (ctx->pdfdebug)
            dmprintf1(ctx->memory, "%c", HexBuf[1]);

        Buffer[index] = (fromhex(HexBuf[0]) << 4) + fromhex(HexBuf[1]);

        if (index++ >= size - 1) {
            NewBuf = (char *)gs_alloc_bytes(ctx->memory, size + 256, "pdfi_read_hexstring");
            if (NewBuf == NULL) {
                gs_free_object(ctx->memory, Buffer, "pdfi_read_hexstring error");
                return_error(gs_error_VMerror);
            }
            memcpy(NewBuf, Buffer, size);
            gs_free_object(ctx->memory, Buffer, "pdfi_read_hexstring");
            Buffer = NewBuf;
            size += 256;
        }
    } while(1);

    if (ctx->pdfdebug)
        dmprintf(ctx->memory, ">");

    code = pdfi_alloc_object(ctx, PDF_STRING, index, (pdf_obj **)&string);
    if (code < 0) {
        gs_free_object(ctx->memory, Buffer, "pdfi_read_name error");
        return code;
    }
    memcpy(string->data, Buffer, index);
    gs_free_object(ctx->memory, Buffer, "pdfi_read_hexstring");

    code = pdfi_push(ctx, (pdf_obj *)string);
    if (code < 0)
        pdfi_free_namestring((pdf_obj *)string);

    return code;
}

static int pdfi_read_string(pdf_context *ctx, pdf_stream *s)
{
    char *Buffer, *NewBuf = NULL, octal[3];
    unsigned short index = 0;
    short bytes = 0;
    uint32_t size = 256;
    pdf_string *string = NULL;
    int code, octal_index = 0, nesting = 0;
    bool escape = false, skip_eol = false, exit_loop = false;

    Buffer = (char *)gs_alloc_bytes(ctx->memory, size, "pdfi_read_string");
    if (Buffer == NULL)
        return_error(gs_error_VMerror);

    do {
        bytes = pdfi_read_bytes(ctx, (byte *)&Buffer[index], 1, 1, s);

        if (bytes == 0 && s->eof)
            break;
        if (bytes <= 0) {
            Buffer[index] = 0x00;
            break;
        }

        if (skip_eol) {
            if (Buffer[index] == 0x0a || Buffer[index] == 0x0d)
                continue;
            skip_eol = false;
        }

        if (escape) {
            escape = false;
            if (Buffer[index] == 0x0a || Buffer[index] == 0x0d) {
                skip_eol = true;
                continue;
            }
            if (octal_index) {
                byte dummy[2];
                dummy[0] = '\\';
                dummy[1] = Buffer[index];
                code = pdfi_unread(ctx, s, dummy, 2);
                if (code < 0) {
                    gs_free_object(ctx->memory, Buffer, "pdfi_read_string");
                    return code;
                }
                Buffer[index] = octal[0];
                if (octal_index == 2)
                    Buffer[index] = (Buffer[index] * 8) + octal[1];
                octal_index = 0;
            } else {
                switch (Buffer[index]) {
                    case 'n':
                        Buffer[index] = 0x0a;
                        break;
                    case 'r':
                        Buffer[index] = 0x0a;
                        break;
                    case 't':
                        Buffer[index] = 0x09;
                        break;
                    case 'b':
                        Buffer[index] = 0x07;
                        break;
                    case 'f':
                        Buffer[index] = 0x0c;
                        break;
                    case '(':
                    case ')':
                    case '\\':
                        break;
                    default:
                        if (Buffer[index] >= 0x30 && Buffer[index] <= 0x37) {
                            octal[octal_index] = Buffer[index] - 0x30;
                            octal_index++;
                            continue;
                        }
                        gs_free_object(ctx->memory, Buffer, "pdfi_read_string");
                        return_error(gs_error_syntaxerror);
                        break;
                }
            }
        } else {
            switch(Buffer[index]) {
                case 0x0a:
                case 0x0d:
                    if (octal_index != 0) {
                        code = pdfi_unread(ctx, s, (byte *)&Buffer[index], 1);
                        if (code < 0) {
                            gs_free_object(ctx->memory, Buffer, "pdfi_read_string");
                            return code;
                        }
                        Buffer[index] = octal[0];
                        if (octal_index == 2)
                            Buffer[index] = (Buffer[index] * 8) + octal[1];
                        octal_index = 0;
                    } else {
                        Buffer[index] = 0x0a;
                        skip_eol = true;
                    }
                    break;
                case ')':
                    if (octal_index != 0) {
                        code = pdfi_unread(ctx, s, (byte *)&Buffer[index], 1);
                        if (code < 0) {
                            gs_free_object(ctx->memory, Buffer, "pdfi_read_string");
                            return code;
                        }
                        Buffer[index] = octal[0];
                        if (octal_index == 2)
                            Buffer[index] = (Buffer[index] * 8) + octal[1];
                        octal_index = 0;
                    } else {
                        if (nesting == 0) {
                            Buffer[index] = 0x00;
                            exit_loop = true;
                        } else
                            nesting--;
                    }
                    break;
                case '\\':
                    escape = true;
                    continue;
                case '(':
                    ctx->pdf_errors |= E_PDF_UNESCAPEDSTRING;
                    nesting++;
                    break;
                default:
                    if (octal_index) {
                        if (Buffer[index] >= 0x30 && Buffer[index] <= 0x37) {
                            octal[octal_index] = Buffer[index] - 0x30;
                            if (++octal_index < 3)
                                continue;
                            Buffer[index] = (octal[0] * 64) + (octal[1] * 8) + octal[2];
                            octal_index = 0;
                        } else {
                            code = pdfi_unread(ctx, s, (byte *)&Buffer[index], 1);
                            if (code < 0) {
                                gs_free_object(ctx->memory, Buffer, "pdfi_read_string");
                                return code;
                            }
                            Buffer[index] = octal[0];
                            if (octal_index == 2)
                                Buffer[index] = (Buffer[index] * 8) + octal[1];
                            octal_index = 0;
                        }
                    }
                    break;
            }
        }

        if (exit_loop)
            break;

        if (index++ >= size - 1) {
            NewBuf = (char *)gs_alloc_bytes(ctx->memory, size + 256, "pdfi_read_string");
            if (NewBuf == NULL) {
                gs_free_object(ctx->memory, Buffer, "pdfi_read_string error");
                return_error(gs_error_VMerror);
            }
            memcpy(NewBuf, Buffer, size);
            gs_free_object(ctx->memory, Buffer, "pdfi_read_string");
            Buffer = NewBuf;
            size += 256;
        }
    } while(1);

    code = pdfi_alloc_object(ctx, PDF_STRING, index, (pdf_obj **)&string);
    if (code < 0) {
        gs_free_object(ctx->memory, Buffer, "pdfi_read_name error");
        return code;
    }
    memcpy(string->data, Buffer, index);

    gs_free_object(ctx->memory, Buffer, "pdfi_read_string");

    if (ctx->pdfdebug) {
        int i;
        dmprintf(ctx->memory, " (");
        for (i=0;i<string->length;i++)
            dmprintf1(ctx->memory, "%c", string->data[i]);
        dmprintf(ctx->memory, ")");
    }

    code = pdfi_push(ctx, (pdf_obj *)string);
    if (code < 0)
        pdfi_free_namestring((pdf_obj *)string);

    return code;
}

static int pdfi_array_from_stack(pdf_context *ctx)
{
    uint64_t index = 0;
    pdf_array *a = NULL;
    pdf_obj *o;
    int code;

    code = pdfi_count_to_mark(ctx, &index);
    if (code < 0)
        return code;

    code = pdfi_alloc_object(ctx, PDF_ARRAY, index, (pdf_obj **)&a);
    if (code < 0)
        return code;

    a->entries = index;

    while (index) {
        o = ctx->stack_top[-1];
        a->values[--index] = o;
        pdfi_countup(o);
        pdfi_pop(ctx, 1);
    }

    code = pdfi_clear_to_mark(ctx);
    if (code < 0)
        return code;

    if (ctx->pdfdebug)
        dmprintf (ctx->memory, " ]\n");

    code = pdfi_push(ctx, (pdf_obj *)a);
    if (code < 0)
        pdfi_free_array((pdf_obj *)a);

    return code;
}

int pdfi_read_dict(pdf_context *ctx, pdf_stream *s)
{
    int code, depth;

    code = pdfi_read_token(ctx, s);
    if (code < 0)
        return code;
    if (ctx->stack_top[-1]->type != PDF_DICT_MARK)
        return_error(gs_error_typecheck);
    depth = ctx->stack_top - ctx->stack_bot;

    do {
        code = pdfi_read_token(ctx, s);
        if (code < 0)
            return code;
    } while(ctx->stack_top - ctx->stack_bot > depth);
    return 0;
}


static int pdfi_skip_comment(pdf_context *ctx, pdf_stream *s)
{
    byte Buffer;
    short bytes = 0;

    if (ctx->pdfdebug)
        dmprintf (ctx->memory, " %%");

    do {
        bytes = pdfi_read_bytes(ctx, (byte *)&Buffer, 1, 1, s);
        if (bytes < 0)
            return_error(gs_error_ioerror);

        if (bytes > 0) {
            if (ctx->pdfdebug)
                dmprintf1 (ctx->memory, " %c", Buffer);

            if ((Buffer == 0x0A) || (Buffer == 0x0D)) {
                break;
            }
        }
    } while (bytes);
    return 0;
}

/* This function is slightly misnamed, for some keywords we do
 * indeed read the keyword and return a PDF_KEYWORD object, but
 * for null, true, false and R we create an appropriate object
 * of that type (PDF_NULL, PDF_BOOL or PDF_INDIRECT_REF)
 * and return it instead.
 */
static int pdfi_read_keyword(pdf_context *ctx, pdf_stream *s)
{
    byte Buffer[256];
    unsigned short index = 0;
    short bytes = 0;
    int code;
    pdf_keyword *keyword;

    skip_white(ctx, s);

    do {
        bytes = pdfi_read_bytes(ctx, (byte *)&Buffer[index], 1, 1, s);
        if (bytes < 0)
            return_error(gs_error_ioerror);

        if (bytes > 0) {
            if (iswhite(Buffer[index])) {
                break;
            } else {
                if (isdelimiter(Buffer[index])) {
                    pdfi_unread(ctx, s, (byte *)&Buffer[index], 1);
                    break;
                }
            }
            index++;
        }
    } while (bytes && index < 255);

    if (index >= 255 || index == 0) {
        if (ctx->pdfstoponerror)
            return_error(gs_error_syntaxerror);
        strcpy((char *)Buffer, "KEYWORD_TOO_LONG");
        index = 16;
    }

    /* NB The code below uses 'Buffer', not the data stored in keyword->data to compare strings */
    Buffer[index] = 0x00;

    code = pdfi_alloc_object(ctx, PDF_KEYWORD, index, (pdf_obj **)&keyword);
    if (code < 0)
        return code;

    memcpy(keyword->data, Buffer, index);
    pdfi_countup(keyword);

    if (ctx->pdfdebug)
        dmprintf1(ctx->memory, " %s\n", Buffer);

    switch(Buffer[0]) {
        case 'K':
            if (keyword->length == 16 && memcmp(keyword->data, "KEYWORD_TOO_LONG", 16) == 0) {
                keyword->key = PDF_INVALID_KEY;
            }
            break;
        case 'R':
            if (keyword->length == 1){
                pdf_indirect_ref *o;
                uint64_t obj_num;
                uint32_t gen_num;

                pdfi_countdown(keyword);

                if(ctx->stack_top - ctx->stack_bot < 2) {
                    pdfi_countdown(keyword);
                    return_error(gs_error_stackunderflow);
                }

                if(((pdf_obj *)ctx->stack_top[-1])->type != PDF_INT || ((pdf_obj *)ctx->stack_top[-2])->type != PDF_INT) {
                    pdfi_countdown(keyword);
                    return_error(gs_error_typecheck);
                }

                gen_num = ((pdf_num *)ctx->stack_top[-1])->value.i;
                pdfi_pop(ctx, 1);
                obj_num = ((pdf_num *)ctx->stack_top[-1])->value.i;
                pdfi_pop(ctx, 1);

                code = pdfi_alloc_object(ctx, PDF_INDIRECT, 0, (pdf_obj **)&o);
                if (code < 0) {
                    pdfi_countdown(keyword);
                    return code;
                }

                o->ref_generation_num = gen_num;
                o->ref_object_num = obj_num;

                code = pdfi_push(ctx, (pdf_obj *)o);
                if (code < 0) {
                    pdfi_countdown(keyword);
                    pdfi_free_object((pdf_obj *)o);
                }
                return code;
            }
            break;
        case 'e':
            if (keyword->length == 9 && memcmp((const char *)Buffer, "endstream", 9) == 0)
                keyword->key = PDF_ENDSTREAM;
            else {
                if (keyword->length == 6 && memcmp((const char *)Buffer, "endobj", 6) == 0)
                    keyword->key = PDF_ENDOBJ;
            }
            break;
        case 'o':
            if (keyword->length == 3 && memcmp((const char *)Buffer, "obj", 3) == 0)
                keyword->key = PDF_OBJ;
            break;
        case 's':
            if (keyword->length == 6 && memcmp((const char *)Buffer, "stream", 6) == 0){
                keyword->key = PDF_STREAM;
                code = skip_eol(ctx, s);
                if (code < 0) {
                    pdfi_countdown(keyword);
                    return code;
                }
            }
            else {
                if (keyword->length == 9 && memcmp((const char *)Buffer, "startxref", 9) == 0)
                    keyword->key = PDF_STARTXREF;
            }
            break;
        case 't':
            if (keyword->length == 4 && memcmp((const char *)Buffer, "true", 4) == 0) {
                pdf_bool *o;

                pdfi_countdown(keyword);

                code = pdfi_alloc_object(ctx, PDF_BOOL, 0, (pdf_obj **)&o);
                if (code < 0)
                    return code;

                o->value = true;

                code = pdfi_push(ctx, (pdf_obj *)o);
                if (code < 0)
                    pdfi_free_object((pdf_obj *)o);
                return code;
            }
            else {
                if (keyword->length == 7 && memcmp((const char *)Buffer, "trailer", 7) == 0)
                    keyword->key = PDF_TRAILER;
            }
            break;
        case 'f':
            if (keyword->length == 5 && memcmp((const char *)Buffer, "false", 5) == 0)
            {
                pdf_bool *o;

                pdfi_countdown(keyword);

                code = pdfi_alloc_object(ctx, PDF_BOOL, 0, (pdf_obj **)&o);
                if (code < 0)
                    return code;

                o->value = false;

                code = pdfi_push(ctx, (pdf_obj *)o);
                if (code < 0)
                    pdfi_free_object((pdf_obj *)o);
                return code;
            }
            break;
        case 'n':
            if (keyword->length == 4 && memcmp((const char *)Buffer, "null", 4) == 0){
                pdf_obj *o;

                pdfi_countdown(keyword);

                code = pdfi_alloc_object(ctx, PDF_NULL, 0, &o);
                if (code < 0)
                    return code;

                code = pdfi_push(ctx, o);
                if (code < 0)
                    pdfi_free_object((pdf_obj *)o);
                return code;
            }
            break;
        case 'x':
            if (keyword->length == 4 && memcmp((const char *)Buffer, "xref", 4) == 0)
                keyword->key = PDF_XREF;
            break;
    }

    code = pdfi_push(ctx, (pdf_obj *)keyword);
    pdfi_countdown(keyword);

    return code;
}

/* This function reads from the given stream, at the current offset in the stream,
 * a single PDF 'token' and returns it on the stack.
 */
int pdfi_read_token(pdf_context *ctx, pdf_stream *s)
{
    int32_t bytes = 0;
    char Buffer[256];
    int code;

    skip_white(ctx, s);

    bytes = pdfi_read_bytes(ctx, (byte *)Buffer, 1, 1, s);
    if (bytes < 0)
        return (gs_error_ioerror);
    if (bytes == 0 && s->eof)
        return 0;

    switch(Buffer[0]) {
        case 0x30:
        case 0x31:
        case 0x32:
        case 0x33:
        case 0x34:
        case 0x35:
        case 0x36:
        case 0x37:
        case 0x38:
        case 0x39:
        case '+':
        case '-':
        case '.':
            pdfi_unread(ctx, s, (byte *)&Buffer[0], 1);
            code = pdfi_read_num(ctx, s);
            if (code < 0)
                return code;
            break;
        case '/':
            return pdfi_read_name(ctx, s);
            break;
        case '<':
            bytes = pdfi_read_bytes(ctx, (byte *)&Buffer[1], 1, 1, s);
            if (bytes <= 0)
                return (gs_error_ioerror);
            if (iswhite(Buffer[1])) {
                code = skip_white(ctx, s);
                if (code < 0)
                    return code;
                bytes = pdfi_read_bytes(ctx, (byte *)&Buffer[1], 1, 1, s);
            }
            if (Buffer[1] == '<') {
                if (ctx->pdfdebug)
                    dmprintf (ctx->memory, " <<\n");
                return pdfi_mark_stack(ctx, PDF_DICT_MARK);
            } else {
                if (Buffer[1] == '>') {
                    pdfi_unread(ctx, s, (byte *)&Buffer[1], 1);
                    return pdfi_read_hexstring(ctx, s);
                } else {
                    if (ishex(Buffer[1])) {
                        pdfi_unread(ctx, s, (byte *)&Buffer[1], 1);
                        return pdfi_read_hexstring(ctx, s);
                    }
                    else
                        return_error(gs_error_syntaxerror);
                }
            }
            break;
        case '>':
            bytes = pdfi_read_bytes(ctx, (byte *)&Buffer[1], 1, 1, s);
            if (bytes <= 0)
                return (gs_error_ioerror);
            if (Buffer[1] == '>')
                return pdfi_dict_from_stack(ctx);
            else
                return_error(gs_error_syntaxerror);
            break;
        case '(':
            return pdfi_read_string(ctx, s);
            break;
        case '[':
            if (ctx->pdfdebug)
                dmprintf (ctx->memory, "[");
            return pdfi_mark_stack(ctx, PDF_ARRAY_MARK);
            break;
        case ']':
            code = pdfi_array_from_stack(ctx);
            if (code < 0) {
                if (code == gs_error_VMerror || code == gs_error_ioerror || ctx->pdfstoponerror)
                    return code;
                pdfi_clearstack(ctx);
                return pdfi_read_token(ctx, s);
            }
            break;
        case '{':
            if (ctx->pdfdebug)
                dmprintf (ctx->memory, "{");
            return pdfi_mark_stack(ctx, PDF_PROC_MARK);
            break;
        case '}':
            pdfi_clear_to_mark(ctx);
            return pdfi_read_token(ctx, s);
            break;
        case '%':
            pdfi_skip_comment(ctx, s);
            return pdfi_read_token(ctx, s);
            break;
        default:
            if (isdelimiter(Buffer[0])) {
                if (ctx->pdfstoponerror)
                    return_error(gs_error_syntaxerror);
                return pdfi_read_token(ctx, s);
            }
            pdfi_unread(ctx, s, (byte *)&Buffer[0], 1);
            return pdfi_read_keyword(ctx, s);
            break;
    }
    return 0;
}

int pdfi_read_object(pdf_context *ctx, pdf_stream *s)
{
    int code = 0;
    uint64_t objnum = 0, gen = 0;
    int64_t i;
    pdf_keyword *keyword = NULL;

    /* An object consists of 'num gen obj' followed by a token, follwed by an endobj
     * A stream dictionary might have a 'stream' instead of an 'endobj', in which case we
     * want to deal with it specially by getting the Length, jumping to the end and checking
     * for an endobj. Or not, possibly, because it would be slow.
     */
    code = pdfi_read_token(ctx, s);
    if (code < 0)
        return code;
    if (((pdf_obj *)ctx->stack_top[-1])->type != PDF_INT) {
        pdfi_pop(ctx, 1);
        return_error(gs_error_typecheck);
    }
    objnum = ((pdf_num *)ctx->stack_top[-1])->value.i;
    pdfi_pop(ctx, 1);

    code = pdfi_read_token(ctx, s);
    if (code < 0)
        return code;
    if (((pdf_obj *)ctx->stack_top[-1])->type != PDF_INT) {
        pdfi_pop(ctx, 1);
        return_error(gs_error_typecheck);
    }
    gen = ((pdf_num *)ctx->stack_top[-1])->value.i;
    pdfi_pop(ctx, 1);

    code = pdfi_read_token(ctx, s);
    if (code < 0)
        return code;
    if (((pdf_obj *)ctx->stack_top[-1])->type != PDF_KEYWORD) {
        pdfi_pop(ctx, 1);
        return_error(gs_error_typecheck);
    }
    keyword = ((pdf_keyword *)ctx->stack_top[-1]);
    if (keyword->key != PDF_OBJ) {
        pdfi_pop(ctx, 1);
        return_error(gs_error_syntaxerror);
    }
    pdfi_pop(ctx, 1);

    code = pdfi_read_token(ctx, s);
    if (code < 0)
        return code;

    do {
        code = pdfi_read_token(ctx, s);
        if (code < 0)
            return code;
        if (s->eof)
            return_error(gs_error_syntaxerror);
    }while (ctx->stack_top[-1]->type != PDF_KEYWORD);

    keyword = ((pdf_keyword *)ctx->stack_top[-1]);
    if (keyword->key == PDF_ENDOBJ) {
        pdf_obj *o;

        if (ctx->stack_top - ctx->stack_bot < 2)
            return_error(gs_error_stackunderflow);

        o = ctx->stack_top[-2];

        pdfi_pop(ctx, 1);

        o->object_num = objnum;
        o->generation_num = gen;
        code = pdfi_add_to_cache(ctx, o);
        return code;
    }
    if (keyword->key == PDF_STREAM) {
        pdf_dict *d = (pdf_dict *)ctx->stack_top[-2];
        gs_offset_t offset;

        skip_eol(ctx, ctx->main_stream);

        offset = pdfi_unread_tell(ctx);

        pdfi_pop(ctx, 1);

        if (ctx->stack_top - ctx->stack_bot < 1)
            return_error(gs_error_stackunderflow);

        d = (pdf_dict *)ctx->stack_top[-1];

        if (d->type != PDF_DICT) {
            pdfi_pop(ctx, 1);
            return_error(gs_error_syntaxerror);
        }
        d->object_num = objnum;
        d->generation_num = gen;
        d->stream_offset = offset;
        code = pdfi_add_to_cache(ctx, (pdf_obj *)d);
        if (code < 0) {
            pdfi_pop(ctx, 1);
            return code;
        }

        /* This code may be a performance overhead, it simply skips over the stream contents
         * and checks that the stream ends with a 'endstream endobj' pair. We could add a
         * 'go faster' flag for users who are certain their PDF files are well-formed. This
         * could also allow us to skip all kinds of other checking.....
         */

        code = pdfi_dict_get_int(ctx, d, "Length", &i);
        if (code < 0) {
            dmprintf1(ctx->memory, "Stream object %"PRIu64" is missing the mandatory keyword /Length, unable to verify the stream langth.\n", objnum);
            return 0;
        }
        if (i < 0 || (i + offset)> ctx->main_stream_length) {
            dmprintf1(ctx->memory, "Stream object %"PRIu64" has a /Length which, when added to the offset of the object, exceeds the file size.\n", objnum);
            return 0;
        }
        code = pdfi_seek(ctx, ctx->main_stream, i, SEEK_CUR);
        if (code < 0) {
            pdfi_pop(ctx, 1);
            return code;
        }

        code = pdfi_read_token(ctx, s);
        if (ctx->stack_top - ctx->stack_bot < 2) {
            dmprintf1(ctx->memory, "Failed to find a valid object at the end of the stream object %"PRIu64".\n", objnum);
            return 0;
        }

        if (((pdf_obj *)ctx->stack_top[-1])->type != PDF_KEYWORD) {
            dmprintf1(ctx->memory, "Failed to find an 'endstream' keyword at the end of the stream object %"PRIu64".\n", objnum);
            pdfi_pop(ctx, 1);
            return 0;
        }
        keyword = ((pdf_keyword *)ctx->stack_top[-1]);
        if (keyword->key != PDF_ENDSTREAM) {
            dmprintf2(ctx->memory, "Stream object %"PRIu64" has an incorrect /Length of %"PRIu64"\n", objnum, i);
            pdfi_pop(ctx, 1);
            return 0;
        }
        pdfi_pop(ctx, 1);

        code = pdfi_read_token(ctx, s);
        if (code < 0) {
            if (ctx->pdfstoponerror)
                return code;
            else
                /* Something went wrong looking for endobj, but we foudn endstream, so assume
                 * for now that will suffice.
                 */
                return 0;
        }

        if (ctx->stack_top - ctx->stack_bot < 2)
            return_error(gs_error_stackunderflow);

        if (((pdf_obj *)ctx->stack_top[-1])->type != PDF_KEYWORD) {
            pdfi_pop(ctx, 2);
            return_error(gs_error_typecheck);
        }
        keyword = ((pdf_keyword *)ctx->stack_top[-1]);
        if (keyword->key != PDF_ENDOBJ) {
            pdfi_pop(ctx, 2);
            return_error(gs_error_typecheck);
        }
        pdfi_pop(ctx, 1);
        return 0;
    }
    /* Assume that any other keyword means a missing 'endobj' */
    if (!ctx->pdfstoponerror) {
        pdf_obj *o;

        ctx->pdf_errors |= E_PDF_MISSINGENDOBJ;

        if (ctx->stack_top - ctx->stack_bot < 2)
            return_error(gs_error_stackunderflow);

        o = ctx->stack_top[-2];

        pdfi_pop(ctx, 1);

        o->object_num = objnum;
        o->generation_num = gen;
        code = pdfi_add_to_cache(ctx, o);
        return code;
    }
    pdfi_pop(ctx, 2);
    return_error(gs_error_syntaxerror);
}

int pdfi_read_object_of_type(pdf_context *ctx, pdf_stream *s, pdf_obj_type type)
{
    int code;

    code = pdfi_read_object(ctx, s);
    if (code < 0)
        return code;

    if (ctx->stack_top[-1]->type != type) {
        pdfi_pop(ctx, 1);
        return_error(gs_error_typecheck);
    }
    return 0;
}

/* In contrast to the 'read' functions, the 'make' functions create an object with a
 * reference count of 1. This indicates that the caller holds the reference. Thus the
 * caller need not increment the reference count to the object, but must decrement
 * it (pdf_countdown) before exiting.
 */
int pdfi_make_name(pdf_context *ctx, byte *n, uint32_t size, pdf_obj **o)
{
    int code;
    *o = NULL;

    code = pdfi_alloc_object(ctx, PDF_NAME, size, o);
    if (code < 0)
        return code;
    pdfi_countup(*o);

    memcpy(((pdf_name *)*o)->data, n, size);

    return 0;
}

/* Now routines to open a PDF file (and clean up in the event of an error) */

static int pdfi_repair_add_object(pdf_context *ctx, uint64_t obj, uint64_t gen, gs_offset_t offset)
{
    /* Although we can handle object numbers larger than this, on some systems (32-bit Windows)
     * memset is limited to a (signed!) integer for the size of memory to clear. We could deal
     * with this by clearing the memory in blocks, but really, this is almost certainly a
     * corrupted file or something.
     */
    if (obj >= 0x7ffffff / sizeof(xref_entry))
        return_error(gs_error_rangecheck);

    if (ctx->xref_table == NULL) {
        ctx->xref_table = (xref_table *)gs_alloc_bytes(ctx->memory, sizeof(xref_table), "repair xref table");
        if (ctx->xref_table == NULL) {
            return_error(gs_error_VMerror);
        }
        memset(ctx->xref_table, 0x00, sizeof(xref_table));
        ctx->xref_table->xref = (xref_entry *)gs_alloc_bytes(ctx->memory, (obj + 1) * sizeof(xref_entry), "repair xref table");
        if (ctx->xref_table->xref == NULL){
            gs_free_object(ctx->memory, ctx->xref_table, "failed to allocate xref table entries for repair");
            ctx->xref_table = NULL;
            return_error(gs_error_VMerror);
        }
        memset(ctx->xref_table->xref, 0x00, (obj + 1) * sizeof(xref_entry));
        ctx->xref_table->memory = ctx->memory;
        ctx->xref_table->type = PDF_XREF_TABLE;
        ctx->xref_table->xref_size = obj + 1;
#if REFCNT_DEBUG
        ctx->xref_table->UID = ctx->UID++;
        dmprintf1(ctx->memory, "Allocated xref table with UID %"PRIi64"\n", ctx->xref_table->UID);
#endif
        pdfi_countup(ctx->xref_table);
    } else {
        if (ctx->xref_table->xref_size < (obj + 1)) {
            xref_entry *new_xrefs;

            new_xrefs = (xref_entry *)gs_alloc_bytes(ctx->memory, (obj + 1) * sizeof(xref_entry), "read_xref_stream allocate xref table entries");
            if (new_xrefs == NULL){
                pdfi_countdown(ctx->xref_table);
                ctx->xref_table = NULL;
                return_error(gs_error_VMerror);
            }
            memset(new_xrefs, 0x00, (obj + 1) * sizeof(xref_entry));
            memcpy(new_xrefs, ctx->xref_table->xref, ctx->xref_table->xref_size * sizeof(xref_entry));
            gs_free_object(ctx->memory, ctx->xref_table->xref, "reallocated xref entries");
            ctx->xref_table->xref = new_xrefs;
            ctx->xref_table->xref_size = obj + 1;
        }
    }
    ctx->xref_table->xref[obj].compressed = false;
    ctx->xref_table->xref[obj].free = false;
    ctx->xref_table->xref[obj].object_num = obj;
    ctx->xref_table->xref[obj].u.uncompressed.generation_num = gen;
    ctx->xref_table->xref[obj].u.uncompressed.offset = offset;
    return 0;
}

int pdfi_repair_file(pdf_context *ctx)
{
    int code;
    gs_offset_t offset;
    uint64_t object_num = 0, generation_num = 0;
    int i;

    ctx->repaired = true;

    pdfi_clearstack(ctx);

    if(ctx->pdfdebug)
        dmprintf(ctx->memory, "%% Error encountered in opening PDF file, attempting repair\n");

    /* First try to locate a %PDF header. If we can't find one, abort this, the file is too broken
     * and may not even be a PDF file.
     */
    pdfi_seek(ctx, ctx->main_stream, 0, SEEK_SET);
    {
        char Buffer[10], test[] = "%PDF";
        int index = 0;

        do {
            code = pdfi_read_bytes(ctx, (byte *)&Buffer[index], 1, 1, ctx->main_stream);
            if (code < 0)
                return code;
            if (Buffer[index] == test[index])
                index++;
            else
                index = 0;
        } while (index < 4 && ctx->main_stream->eof == false);
        if (memcmp(Buffer, test, 4) != 0)
            return_error(gs_error_undefined);
        pdfi_unread(ctx, ctx->main_stream, (byte *)Buffer, 4);
        pdfi_skip_comment(ctx, ctx->main_stream);
    }
    if (ctx->main_stream->eof == true)
        return_error(gs_error_ioerror);

    /* First pass, identify all the objects of the form x y obj */

    do {
        offset = pdfi_unread_tell(ctx);
        do {
            code = pdfi_read_token(ctx, ctx->main_stream);
            if (code < 0) {
                if (code != gs_error_VMerror && code != gs_error_ioerror) {
                    pdfi_clearstack(ctx);
                    offset = pdfi_unread_tell(ctx);
                    continue;
                } else
                    return code;
            }
            if (ctx->stack_top - ctx->stack_bot > 0) {
                if (ctx->stack_top[-1]->type == PDF_KEYWORD) {
                    pdf_keyword *k = (pdf_keyword *)ctx->stack_top[-1];
                    pdf_num *n;

                    if (k->key == PDF_OBJ) {
                        if (ctx->stack_top - ctx->stack_bot < 3 || ctx->stack_top[-2]->type != PDF_INT || ctx->stack_top[-2]->type != PDF_INT) {
                            pdfi_clearstack(ctx);
                            continue;
                        }
                        n = (pdf_num *)ctx->stack_top[-3];
                        object_num = n->value.i;
                        n = (pdf_num *)ctx->stack_top[-2];
                        generation_num = n->value.i;
                        pdfi_clearstack(ctx);

                        do {
                            code = pdfi_read_token(ctx, ctx->main_stream);
                            if (code < 0) {
                                if (code != gs_error_VMerror && code != gs_error_ioerror)
                                    continue;
                                return code;
                            }
                            if (code == 0 && ctx->main_stream->eof)
                                break;

                            if (ctx->stack_top[-1]->type == PDF_KEYWORD){
                                pdf_keyword *k = (pdf_keyword *)ctx->stack_top[-1];

                                if (k->key == PDF_ENDOBJ) {
                                    code = pdfi_repair_add_object(ctx, object_num, generation_num, offset);
                                    if (code < 0)
                                        return code;
                                    pdfi_clearstack(ctx);
                                    break;
                                } else {
                                    if (k->key == PDF_STREAM) {
                                        char Buffer[10], test[] = "endstream";
                                        int index = 0;

                                        do {
                                            code = pdfi_read_bytes(ctx, (byte *)&Buffer[index], 1, 1, ctx->main_stream);
                                            if (code < 0) {
                                                if (code != gs_error_VMerror && code != gs_error_ioerror)
                                                    continue;
                                                return code;
                                            }
                                            if (Buffer[index] == test[index])
                                                index++;
                                            else
                                                index = 0;
                                        } while (index < 9 && ctx->main_stream->eof == false);
                                        do {
                                            code = pdfi_read_token(ctx, ctx->main_stream);
                                            if (code < 0) {
                                                if (code != gs_error_VMerror && code != gs_error_ioerror)
                                                    continue;
                                                return code;
                                            }
                                            if (ctx->stack_top[-1]->type == PDF_KEYWORD){
                                                pdf_keyword *k = (pdf_keyword *)ctx->stack_top[-1];
                                                if (k->key == PDF_ENDOBJ) {
                                                    code = pdfi_repair_add_object(ctx, object_num, generation_num, offset);
                                                    if (code < 0) {
                                                        if (code != gs_error_VMerror && code != gs_error_ioerror)
                                                            continue;
                                                        return code;
                                                    }
                                                    break;
                                                }
                                            }
                                        }while(ctx->main_stream->eof == false);

                                        pdfi_clearstack(ctx);
                                        break;
                                    } else {
                                        pdfi_clearstack(ctx);
                                        break;
                                    }
                                }
                            }
                        } while(1);
                        break;
                    } else {
                        if (k->key == PDF_ENDOBJ) {
                            code = pdfi_repair_add_object(ctx, object_num, generation_num, offset);
                            if (code < 0)
                                return code;
                            pdfi_clearstack(ctx);
                        } else
                            if (k->key == PDF_STARTXREF) {
                                code = pdfi_read_token(ctx, ctx->main_stream);
                                if (code < 0 && code != gs_error_VMerror && code != gs_error_ioerror)
                                    continue;
                                if (code < 0)
                                    return code;
                                offset = pdfi_unread_tell(ctx);
                                pdfi_clearstack(ctx);
                            } else {
                                offset = pdfi_unread_tell(ctx);
                                pdfi_clearstack(ctx);
                            }
                    }
                }
                if (ctx->stack_top - ctx->stack_bot > 0 && ctx->stack_top[-1]->type != PDF_INT)
                    pdfi_clearstack(ctx);
            }
        } while (ctx->main_stream->eof == false);
    } while(ctx->main_stream->eof == false);

    if (ctx->main_stream->eof) {
        sfclose(ctx->main_stream->s);
        ctx->main_stream->s = sfopen(ctx->filename, "r", ctx->memory);
        if (ctx->main_stream->s == NULL)
            return_error(gs_error_ioerror);
        ctx->main_stream->eof = false;
    }

    /* Second pass, examine every object we have located to see if its an ObjStm */
    if (ctx->xref_table == NULL || ctx->xref_table->xref_size < 1)
        return_error(gs_error_syntaxerror);

    for (i=1;i < ctx->xref_table->xref_size;i++) {
        if (ctx->xref_table->xref[i].object_num != 0) {
            /* At this stage, all the objects we've found must be uncompressed */
            if (ctx->xref_table->xref[i].u.uncompressed.offset > ctx->main_stream_length)
                return_error(gs_error_rangecheck);

            pdfi_seek(ctx, ctx->main_stream, ctx->xref_table->xref[i].u.uncompressed.offset, SEEK_SET);
            do {
                code = pdfi_read_token(ctx, ctx->main_stream);
                if (ctx->main_stream->eof == true || (code < 0 && code != gs_error_ioerror && code != gs_error_VMerror))
                    break;;
                if (code < 0)
                    return code;
                if (ctx->stack_top[-1]->type == PDF_KEYWORD) {
                    pdf_keyword *k = (pdf_keyword *)ctx->stack_top[-1];

                    if (k->key == PDF_OBJ){
                        continue;
                    }
                    if (k->key == PDF_ENDOBJ) {
                        if (ctx->stack_top - ctx->stack_bot > 1) {
                            if (ctx->stack_top[-2]->type == PDF_DICT) {
                                 pdf_dict *d = (pdf_dict *)ctx->stack_top[-2];
                                 pdf_obj *o = NULL;

                                 code = pdfi_dict_get(ctx, d, "Type", &o);
                                 if (code < 0 && code != gs_error_undefined){
                                     pdfi_clearstack(ctx);
                                     return code;
                                 }
                                 if (o != NULL) {
                                     pdf_name *n = (pdf_name *)o;

                                     if (n->type == PDF_NAME) {
                                         if (n->length == 7 && memcmp(n->data, "Catalog", 7) == 0) {
                                             ctx->Root = (pdf_dict *)ctx->stack_top[-2];
                                             pdfi_countup(ctx->Root);
                                         }
                                     }
                                 }
                            }
                        }
                        pdfi_clearstack(ctx);
                        break;
                    }
                    if (k->key == PDF_STREAM) {
                        pdf_dict *d;
                        pdf_name *n;

                        if (ctx->stack_top - ctx->stack_bot <= 1) {
                            pdfi_clearstack(ctx);
                            break;;
                        }
                        d = (pdf_dict *)ctx->stack_top[-2];
                        if (d->type != PDF_DICT) {
                            pdfi_clearstack(ctx);
                            break;;
                        }
                        code = pdfi_dict_get(ctx, d, "Type", (pdf_obj **)&n);
                        if (code < 0) {
                            if (ctx->pdfstoponerror || code == gs_error_VMerror) {
                                pdfi_clearstack(ctx);
                                return code;
                            }
                            pdfi_clearstack(ctx);
                            break;
                        }
                        if (n->type == PDF_NAME) {
                            if (n->length == 6 && memcmp(n->data, "ObjStm", 6) == 0) {
                                int64_t N, obj_num, offset;
                                int j;
                                pdf_stream *compressed_stream;
                                pdf_obj *o;

                                offset = pdfi_unread_tell(ctx);
                                pdfi_seek(ctx, ctx->main_stream, offset, SEEK_SET);
                                code = pdfi_filter(ctx, d, ctx->main_stream, &compressed_stream, false);
                                if (code == 0) {
                                    code = pdfi_dict_get_int(ctx, d, "N", &N);
                                    if (code == 0) {
                                        for (j=0;j < N; j++) {
                                            code = pdfi_read_token(ctx, compressed_stream);
                                            if (code == 0) {
                                                o = ctx->stack_top[-1];
                                                if (((pdf_obj *)o)->type == PDF_INT) {
                                                    obj_num = ((pdf_num *)o)->value.i;
                                                    pdfi_pop(ctx, 1);
                                                    code = pdfi_read_token(ctx, compressed_stream);
                                                    if (code == 0) {
                                                        o = ctx->stack_top[-1];
                                                        if (((pdf_obj *)o)->type == PDF_INT) {
                                                            offset = ((pdf_num *)o)->value.i;
                                                            if (obj_num < 1) {
                                                                pdfi_close_file(ctx, compressed_stream);
                                                                pdfi_clearstack(ctx);
                                                                return_error(gs_error_rangecheck);
                                                            }
                                                            ctx->xref_table->xref[obj_num].compressed = true;
                                                            ctx->xref_table->xref[obj_num].free = false;
                                                            ctx->xref_table->xref[obj_num].object_num = obj_num;
                                                            ctx->xref_table->xref[obj_num].u.compressed.compressed_stream_num = i;
                                                            ctx->xref_table->xref[obj_num].u.compressed.object_index = j;
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                    pdfi_close_file(ctx, compressed_stream);
                                }
                                if (code < 0) {
                                    if (ctx->pdfstoponerror || code == gs_error_VMerror) {
                                        pdfi_clearstack(ctx);
                                        return code;
                                    }
                                }
                            }
                        }
                        pdfi_clearstack(ctx);
                        break;
                    }
                }
            } while (1);
        }
    }

    return 0;
}


int pdfi_read_Root(pdf_context *ctx)
{
    pdf_obj *o, *o1;
    int code;

    if (ctx->pdfdebug)
        dmprintf(ctx->memory, "%% Reading Root dictionary\n");

    code = pdfi_dict_get(ctx, ctx->Trailer, "Root", &o1);
    if (code < 0)
        return code;

    if (o1->type == PDF_INDIRECT) {
        pdf_obj *name;

        code = pdfi_dereference(ctx, ((pdf_indirect_ref *)o1)->ref_object_num,  ((pdf_indirect_ref *)o1)->ref_generation_num, &o);
        pdfi_countdown(o1);
        if (code < 0)
            return code;

        if (o->type != PDF_DICT) {
            pdfi_countdown(o);
            return_error(gs_error_typecheck);
        }

        code = pdfi_make_name(ctx, (byte *)"Root", 4, &name);
        if (code < 0) {
            pdfi_countdown(o);
            return code;
        }
        code = pdfi_dict_put(ctx->Trailer, name, o);
        /* pdfi_make_name created a name with a reference count of 1, the local object
         * is going out of scope, so decrement the reference coutn.
         */
        pdfi_countdown(name);
        if (code < 0) {
            pdfi_countdown(o);
            return code;
        }
        o1 = o;
    } else {
        if (o1->type != PDF_DICT) {
            pdfi_countdown(o1);
            return_error(gs_error_typecheck);
        }
    }

    code = pdfi_dict_get_type(ctx, (pdf_dict *)o1, "Type", PDF_NAME, &o);
    if (code < 0) {
        pdfi_countdown(o1);
        return code;
    }
    if (pdfi_name_strcmp((pdf_name *)o, "Catalog") != 0){
        pdfi_countdown(o);
        pdfi_countdown(o1);
        return_error(gs_error_syntaxerror);
    }
    pdfi_countdown(o);

    if (ctx->pdfdebug)
        dmprintf(ctx->memory, "\n");
    /* We don't pdfi_countdown(o1) now, because we've transferred our
     * reference to the pointer in the pdf_context structure.
     */
    ctx->Root = (pdf_dict *)o1;
    return 0;
}

int pdfi_read_Info(pdf_context *ctx)
{
    pdf_obj *o, *o1;
    int code;

    if (ctx->pdfdebug)
        dmprintf(ctx->memory, "%% Reading Info dictionary\n");

    code = pdfi_dict_get(ctx, ctx->Trailer, "Info", &o1);
    if (code < 0)
        return code;

    if (o1->type == PDF_INDIRECT) {
        pdf_obj *name;

        code = pdfi_dereference(ctx, ((pdf_indirect_ref *)o1)->ref_object_num,  ((pdf_indirect_ref *)o1)->ref_generation_num, &o);
        pdfi_countdown(o1);
        if (code < 0)
            return code;

        if (o->type != PDF_DICT) {
            pdfi_countdown(o);
            return_error(gs_error_typecheck);
        }

        code = pdfi_make_name(ctx, (byte *)"Info", 4, &name);
        if (code < 0) {
            pdfi_countdown(o);
            return code;
        }
        code = pdfi_dict_put(ctx->Trailer, name, o);
        /* pdfi_make_name created a name with a reference count of 1, the local object
         * is going out of scope, so decrement the reference coutn.
         */
        pdfi_countdown(name);
        if (code < 0) {
            pdfi_countdown(o);
            return code;
        }
        o1 = o;
    } else {
        if (o1->type != PDF_DICT) {
            pdfi_countdown(o1);
            return_error(gs_error_typecheck);
        }
    }

    if (ctx->pdfdebug)
        dmprintf(ctx->memory, "\n");
    /* We don't pdfi_countdown(o1) now, because we've transferred our
     * reference to the pointer in the pdf_context structure.
     */
    ctx->Info = (pdf_dict *)o1;
    return 0;
}

int pdfi_read_Pages(pdf_context *ctx)
{
    pdf_obj *o, *o1;
    int code;
    double d;

    if (ctx->pdfdebug)
        dmprintf(ctx->memory, "%% Reading Pages dictionary\n");

    code = pdfi_dict_get(ctx, ctx->Root, "Pages", &o1);
    if (code < 0)
        return code;

    if (o1->type == PDF_INDIRECT) {
        pdf_obj *name;

        code = pdfi_dereference(ctx, ((pdf_indirect_ref *)o1)->ref_object_num,  ((pdf_indirect_ref *)o1)->ref_generation_num, &o);
        pdfi_countdown(o1);
        if (code < 0)
            return code;

        if (o->type != PDF_DICT) {
            pdfi_countdown(o);
            return_error(gs_error_typecheck);
        }

        code = pdfi_make_name(ctx, (byte *)"Pages", 5, &name);
        if (code < 0) {
            pdfi_countdown(o);
            return code;
        }
        code = pdfi_dict_put(ctx->Root, name, o);
        /* pdfi_make_name created a name with a reference count of 1, the local object
         * is going out of scope, so decrement the reference coutn.
         */
        pdfi_countdown(name);
        if (code < 0) {
            pdfi_countdown(o);
            return code;
        }
        o1 = o;
    } else {
        if (o1->type != PDF_DICT) {
            pdfi_countdown(o1);
            return_error(gs_error_typecheck);
        }
    }

    if (ctx->pdfdebug)
        dmprintf(ctx->memory, "\n");

    /* Acrobat allows the Pages Count to be a flaoting point nuber (!) */
    code = pdfi_dict_get_number(ctx, (pdf_dict *)o1, "Count", &d);
    if (code < 0)
        return code;

    if (floor(d) != d) {
        pdfi_countdown(o1);
        return_error(gs_error_rangecheck);
    } else {
        ctx->num_pages = (int)floor(d);
    }

    /* We don't pdfi_countdown(o1) now, because we've transferred our
     * reference to the pointer in the pdf_context structure.
     */
    ctx->Pages = (pdf_dict *)o1;
    return 0;
}

/* Read optional things in from Root */
void
pdfi_read_OptionalRoot(pdf_context *ctx)
{
    pdf_obj *obj = NULL;
    int code;

    if (ctx->pdfdebug)
        dmprintf(ctx->memory, "%% Reading other Root contents\n");

    if (ctx->pdfdebug)
        dmprintf(ctx->memory, "%% OCProperties\n");
    code = pdfi_dict_get_type(ctx, ctx->Root, "OCProperties", PDF_DICT, &obj);
    if (code == 0) {
        ctx->OCProperties = (pdf_dict *)obj;
    } else {
        ctx->OCProperties = NULL;
        if (ctx->pdfdebug)
            dmprintf(ctx->memory, "%% (None)\n");
    }

}

void
pdfi_free_OptionalRoot(pdf_context *ctx)
{
    if (ctx->OCProperties) {
        pdfi_countdown(ctx->OCProperties);
    }
}

int pdfi_get_page_dict(pdf_context *ctx, pdf_dict *d, uint64_t page_num, uint64_t *page_offset, pdf_dict **target, pdf_dict *inherited)
{
    int i, code = 0;
    pdf_array *Kids = NULL;
    pdf_indirect_ref *node = NULL;
    pdf_dict *child = NULL;
    pdf_name *Type = NULL;
    pdf_dict *inheritable = NULL;
    int64_t num;
    double dbl;
    bool known;

    if (ctx->pdfdebug)
        dmprintf1(ctx->memory, "%% Finding page dictionary for page %"PRIi64"\n", page_num + 1);

    /* if we are being passed any inherited values from our parent, copy them now */
    if (inherited != NULL) {
        code = pdfi_alloc_object(ctx, PDF_DICT, inherited->size, (pdf_obj **)&inheritable);
        if (code < 0)
            return code;
        code = pdfi_dict_copy(inheritable, inherited);
        if (code < 0) {
            pdfi_countdown(inheritable);
            return code;
        }
    }

    code = pdfi_dict_get_number(ctx, d, "Count", &dbl);
    if (code < 0) {
        pdfi_countdown(inheritable);
        return code;
    }
    if (dbl != floor(dbl))
        return_error(gs_error_rangecheck);
    num = (int)dbl;

    if (num < 0 || (num + *page_offset) > ctx->num_pages) {
        pdfi_countdown(inheritable);
        return_error(gs_error_rangecheck);
    }
    if (num + *page_offset < page_num) {
        pdfi_countdown(inheritable);
        *page_offset += num;
        return 1;
    }
    /* The requested page is a descendant of this node */

    /* Check for inheritable keys, if we find any copy them to the 'inheritable' dictionary at this level */
    code = pdfi_dict_known(d, "Resources", &known);
    if (code < 0) {
        pdfi_countdown(inheritable);
        return code;
    }
    if (known) {
        pdf_name *Key;
        pdf_obj *object;

        if (inheritable == NULL) {
            code = pdfi_alloc_object(ctx, PDF_DICT, 0, (pdf_obj **)&inheritable);
            if (code < 0)
                return code;
        }

        code = pdfi_make_name(ctx, (byte *)"Resources", 9, (pdf_obj **)&Key);
        if (code < 0) {
            pdfi_countdown(inheritable);
            return code;
        }
        code = pdfi_dict_get(ctx, d, "Resources", &object);
        if (code < 0) {
            pdfi_countdown(Key);
            pdfi_countdown(inheritable);
            return code;
        }
        code = pdfi_dict_put(inheritable, (pdf_obj *)Key, object);
        pdfi_countdown(Key);
        pdfi_countdown(object);
    }
    code = pdfi_dict_known(d, "MediaBox", &known);
    if (code < 0) {
        pdfi_countdown(inheritable);
        return code;
    }
    if (known) {
        pdf_name *Key;
        pdf_obj *object;

        if (inheritable == NULL) {
            code = pdfi_alloc_object(ctx, PDF_DICT, 0, (pdf_obj **)&inheritable);
            if (code < 0)
                return code;
        }

        code = pdfi_make_name(ctx, (byte *)"MediaBox", 8, (pdf_obj **)&Key);
        if (code < 0) {
            pdfi_countdown(inheritable);
            return code;
        }
        code = pdfi_dict_get(ctx, d, "MediaBox", &object);
        if (code < 0) {
            pdfi_countdown(Key);
            pdfi_countdown(inheritable);
            return code;
        }
        code = pdfi_dict_put(inheritable, (pdf_obj *)Key, object);
        pdfi_countdown(Key);
        pdfi_countdown(object);
    }
    code = pdfi_dict_known(d, "CropBox", &known);
    if (code < 0) {
        pdfi_countdown(inheritable);
        return code;
    }
    if (known) {
        pdf_name *Key;
        pdf_obj *object;

        if (inheritable == NULL) {
            code = pdfi_alloc_object(ctx, PDF_DICT, 0, (pdf_obj **)&inheritable);
            if (code < 0)
                return code;
        }

        code = pdfi_make_name(ctx, (byte *)"CropBox", 7, (pdf_obj **)&Key);
        if (code < 0) {
            pdfi_countdown(inheritable);
            return code;
        }
        code = pdfi_dict_get(ctx, d, "CropBox", &object);
        if (code < 0) {
            pdfi_countdown(Key);
            pdfi_countdown(inheritable);
            return code;
        }
        code = pdfi_dict_put(inheritable, (pdf_obj *)Key, object);
        pdfi_countdown(Key);
        pdfi_countdown(object);
    }
    code = pdfi_dict_known(d, "Rotate", &known);
    if (code < 0) {
        pdfi_countdown(inheritable);
        return code;
    }
    if (known) {
        pdf_name *Key;
        pdf_obj *object;

        if (inheritable == NULL) {
            code = pdfi_alloc_object(ctx, PDF_DICT, 0, (pdf_obj **)&inheritable);
            if (code < 0)
                return code;
        }

        code = pdfi_make_name(ctx, (byte *)"Rotate", 6, (pdf_obj **)&Key);
        if (code < 0) {
            pdfi_countdown(inheritable);
            return code;
        }
        code = pdfi_dict_get(ctx, d, "Rotate", &object);
        if (code < 0) {
            pdfi_countdown(Key);
            pdfi_countdown(inheritable);
            return code;
        }
        code = pdfi_dict_put(inheritable, (pdf_obj *)Key, object);
        pdfi_countdown(Key);
        pdfi_countdown(object);
    }

    /* Get the Kids array */
    code = pdfi_dict_get_type(ctx, d, "Kids", PDF_ARRAY, (pdf_obj **)&Kids);
    if (code < 0) {
        pdfi_countdown(inheritable);
        pdfi_countdown(Kids);
        return code;
    }

    /* Check each entry in the Kids array */
    for (i = 0;i < Kids->entries;i++) {
        code = pdfi_array_get(Kids, i, (pdf_obj **)&node);
        if (code < 0) {
            pdfi_countdown(inheritable);
            pdfi_countdown(Kids);
            return code;
        }
        if (node->type != PDF_INDIRECT && node->type != PDF_DICT) {
            pdfi_countdown(inheritable);
            pdfi_countdown(Kids);
            pdfi_countdown(node);
            return_error(gs_error_typecheck);
        }
        if (node->type == PDF_INDIRECT) {
            code = pdfi_dereference(ctx, node->ref_object_num, node->ref_generation_num, (pdf_obj **)&child);
            if (code < 0) {
                pdfi_countdown(inheritable);
                pdfi_countdown(Kids);
                pdfi_countdown(node);
                return code;
            }
            if (child->type != PDF_DICT) {
                pdfi_countdown(inheritable);
                pdfi_countdown(Kids);
                pdfi_countdown(node);
                pdfi_countdown(child);
                return_error(gs_error_typecheck);
            }
            /* If its an intermediate node, store it in the page_table, if its a leaf node
             * then don't store it. Instead we create a special dictionary of our own which
             * has a /Type of /PageRef and a /PageRef key which is the indirect reference
             * to the page. However in this case we pass on the actual page dictionary to
             * the Kids processing below. If we didn't then we'd fall foul of the loop
             * detection by dereferencing the same object twice.
             * This is tedious, but it means we don't store all the page dictionaries in
             * the Pages tree, because page dictionaries can be large and we generally
             * only use them once. If processed in order we only dereference each page
             * dictionary once, any other order will dereference each page twice. (or more
             * if we render the same page multiple times).
             */
            code = pdfi_dict_get_type(ctx, child, "Type", PDF_NAME, (pdf_obj **)&Type);
            if (code < 0) {
                pdfi_countdown(inheritable);
                pdfi_countdown(Kids);
                pdfi_countdown(child);
                pdfi_countdown(node);
                return code;
            }
            if (pdfi_name_strcmp(Type, "Pages") == 0) {
                code = pdfi_array_put(Kids, i, (pdf_obj *)child);
                if (code < 0) {
                    pdfi_countdown(inheritable);
                    pdfi_countdown(Kids);
                    pdfi_countdown(child);
                    pdfi_countdown(Type);
                    pdfi_countdown(node);
                    return code;
                }
            }
            if (pdfi_name_strcmp(Type, "Page") == 0) {
                /* Make a 'PageRef' entry (just stores an indirect reference to the actual page)
                 * and store that in the Kids array for future reference. But pass on the
                 * dereferenced Page dictionary, in case this is the target page.
                 */
                pdf_dict *leaf_dict = NULL;
                pdf_name *Key = NULL, *Key1 = NULL;

                code = pdfi_alloc_object(ctx, PDF_DICT, 0, (pdf_obj **)&leaf_dict);
                if (code == 0) {
                    code = pdfi_make_name(ctx, (byte *)"PageRef", 7, (pdf_obj **)&Key);
                    if (code == 0) {
                        code = pdfi_dict_put(leaf_dict, (pdf_obj *)Key, (pdf_obj *)node);
                        if (code == 0){
                            code = pdfi_make_name(ctx, (byte *)"Type", 4, (pdf_obj **)&Key);
                            if (code == 0){
                                code = pdfi_make_name(ctx, (byte *)"PageRef", 7, (pdf_obj **)&Key1);
                                if (code == 0) {
                                    code = pdfi_dict_put(leaf_dict, (pdf_obj *)Key, (pdf_obj *)Key1);
                                    if (code == 0)
                                        code = pdfi_array_put(Kids, i, (pdf_obj *)leaf_dict);
                                }
                            }
                        }
                    }
                }
                pdfi_countdown(Key);
                pdfi_countdown(Key1);
                if (code < 0) {
                    pdfi_countdown(inheritable);
                    pdfi_countdown(Kids);
                    pdfi_countdown(child);
                    pdfi_countdown(Type);
                    pdfi_countdown(node);
                    return code;
                }
            }
            pdfi_countdown(Type);
            pdfi_countdown(node);
        } else {
            child = (pdf_dict *)node;
        }
        /* Check the type, if its a Pages entry, then recurse. If its a Page entry, is it the one we want */
        code = pdfi_dict_get_type(ctx, child, "Type", PDF_NAME, (pdf_obj **)&Type);
        if (code == 0) {
            if (pdfi_name_strcmp(Type, "Pages") == 0) {
                code = pdfi_dict_get_number(ctx, child, "Count", &dbl);
                if (code == 0) {
                    if (dbl != floor(dbl))
                        return_error(gs_error_rangecheck);
                    num = (int)dbl;
                    if (num < 0 || (num + *page_offset) > ctx->num_pages) {
                        code = gs_error_rangecheck;
                    } else {
                        if (num + *page_offset <= page_num) {
                            pdfi_countdown(child);
                            child = NULL;
                            pdfi_countdown(Type);
                            Type = NULL;
                            *page_offset += num;
                        } else {
                            code = pdfi_get_page_dict(ctx, child, page_num, page_offset, target, inheritable);
                            pdfi_countdown(Type);
                            pdfi_countdown(Kids);
                            pdfi_countdown(child);
                            return code;
                        }
                    }
                }
            } else {
                if (pdfi_name_strcmp(Type, "PageRef") == 0) {
                    pdfi_countdown(Type);
                    Type = NULL;
                    if ((*page_offset) == page_num) {
                        pdf_indirect_ref *o = NULL;
                        pdf_dict *d = NULL;
                        code = pdfi_dict_get(ctx, child, "PageRef", (pdf_obj **)&o);
                        if (code == 0) {
                            code = pdfi_dereference(ctx, o->ref_object_num, o->ref_generation_num, (pdf_obj **)&d);
                            if (code == 0) {
                                if (inheritable != NULL) {
                                    code = pdfi_merge_dicts(d, inheritable);
                                    pdfi_countdown(inheritable);
                                    inheritable = NULL;
                                }
                                if (code == 0) {
                                    pdfi_countdown(Kids);
                                    *target = d;
                                    pdfi_countup(*target);
                                    pdfi_countdown(d);
                                    return 0;
                                }
                            }
                        }
                    } else {
                        *page_offset += 1;
                        pdfi_countdown(child);
                    }
                } else {
                    if (pdfi_name_strcmp(Type, "Page") == 0) {
                        pdfi_countdown(Type);
                        Type = NULL;
                        if ((*page_offset) == page_num) {
                            if (inheritable != NULL) {
                                code = pdfi_merge_dicts(child, inheritable);
                            }
                            if (code == 0) {
                                pdfi_countdown(inheritable);
                                pdfi_countdown(Kids);
                                *target = child;
                                pdfi_countup(*target);
                                pdfi_countdown(child);
                                return 0;
                            }
                        } else {
                            *page_offset += 1;
                            pdfi_countdown(child);
                        }
                    } else
                        code = gs_error_typecheck;
                }
            }
        }
        if (code < 0) {
            pdfi_countdown(inheritable);
            pdfi_countdown(Kids);
            pdfi_countdown(child);
            pdfi_countdown(Type);
            return code;
        }
    }
    /* Positive return value indicates we did not find the target below this node, try the next one */
    pdfi_countdown(inheritable);
    return 1;
}

static int split_bogus_operator(pdf_context *ctx)
{
    /* FIXME Need to write this, place holder for now */
    return 0;
}

#define K1(a) (a)
#define K2(a, b) ((a << 8) + b)
#define K3(a, b, c) ((a << 16) + (b << 8) + c)

static int pdfi_interpret_stream_operator(pdf_context *ctx, pdf_stream *source, pdf_dict *stream_dict, pdf_dict *page_dict)
{
    pdf_keyword *keyword = (pdf_keyword *)ctx->stack_top[-1];
    uint32_t op = 0;
    int i, code = 0;

    if (keyword->length > 3) {
        /* This means we either have a corrupted or illegal operator. The most
         * usual corruption is two concatented operators (eg QBT instead of Q BT)
         * I plan to tackle this by trying to see if I can make two or more operators
         * out of the mangled one. Note this will also be done below in the 'default'
         * case where we don't recognise a keyword with 3 or fewer characters.
         */
        code = split_bogus_operator(ctx);
        if (code < 0)
            return code;
    } else {
        for (i=0;i < keyword->length;i++) {
            op = (op << 8) + keyword->data[i];
        }
        switch(op) {
            case K1('b'):           /* closepath, fill, stroke */
                pdfi_pop(ctx, 1);
                code = pdfi_b(ctx);
                break;
            case K1('B'):           /* fill, stroke */
                pdfi_pop(ctx, 1);
                code = pdfi_B(ctx);
                break;
            case K2('b','*'):       /* closepath, eofill, stroke */
                pdfi_pop(ctx, 1);
                code = pdfi_b_star(ctx);
                break;
            case K2('B','*'):       /* eofill, stroke */
                pdfi_pop(ctx, 1);
                code = pdfi_B_star(ctx);
                break;
            case K2('B','I'):       /* begin inline image */
                pdfi_pop(ctx, 1);
                code = pdfi_BI(ctx);
                break;
            case K3('B','D','C'):   /* begin marked content sequence with property list */
                pdfi_pop(ctx, 1);
                if (ctx->stack_top - ctx->stack_bot >= 2) {
                    pdfi_pop(ctx, 2);
                } else
                    pdfi_clearstack(ctx);
                break;
            case K3('B','M','C'):   /* begin marked content sequence */
                pdfi_pop(ctx, 1);
                if (ctx->stack_top - ctx->stack_bot >= 1) {
                    pdfi_pop(ctx, 1);
                } else
                    pdfi_clearstack(ctx);
                break;
            case K2('B','T'):       /* begin text */
            case K2('B','X'):       /* begin compatibility section */
                pdfi_pop(ctx, 1);
                break;
            case K1('c'):           /* curveto */
                pdfi_pop(ctx, 1);
                code = pdfi_curveto(ctx);
                break;
            case K2('c','m'):       /* concat */
                pdfi_pop(ctx, 1);
                code = pdfi_concat(ctx);
                break;
            case K2('C','S'):       /* set stroke colour space */
                pdfi_pop(ctx, 1);
                code = pdfi_setstrokecolor_space(ctx, stream_dict, page_dict);
                break;
            case K2('c','s'):       /* set non-stroke colour space */
                pdfi_pop(ctx, 1);
                code = pdfi_setfillcolor_space(ctx, stream_dict, page_dict);
                break;
                break;
            case K1('d'):           /* set dash params */
                pdfi_pop(ctx, 1);
                code = pdfi_setdash(ctx);
                break;
            case K2('E','I'):       /* end inline image */
                pdfi_pop(ctx, 1);
                code = pdfi_EI(ctx);
                break;
            case K2('d','0'):       /* set type 3 font glyph width */
                pdfi_pop(ctx, 1);
                code = pdfi_d0(ctx);
                break;
            case K2('d','1'):       /* set type 3 font glyph width and bounding box */
                pdfi_pop(ctx, 1);
                code = pdfi_d1(ctx);
                break;
            case K2('D','o'):       /* invoke named XObject */
                pdfi_pop(ctx, 1);
                code = pdfi_Do(ctx, stream_dict, page_dict);
                break;
            case K2('D','P'):       /* define marked content point with property list */
                pdfi_pop(ctx, 1);
                if (ctx->stack_top - ctx->stack_bot >= 2) {
                    pdfi_pop(ctx, 2);
                } else
                    pdfi_clearstack(ctx);
                break;
            case K3('E','M','C'):   /* end marked content sequence */
            case K2('E','T'):       /* end text */
            case K2('E','X'):       /* end compatibility section */
                pdfi_pop(ctx, 1);
                break;
            case K1('f'):           /* fill */
                pdfi_pop(ctx, 1);
                code = pdfi_fill(ctx);
                break;
            case K1('F'):           /* fill (obselete operator) */
                pdfi_pop(ctx, 1);
                code = pdfi_fill(ctx);
                break;
            case K2('f','*'):       /* eofill */
                pdfi_pop(ctx, 1);
                code = pdfi_eofill(ctx);
                break;
            case K1('G'):           /* setgray for stroke */
                pdfi_pop(ctx, 1);
                code = pdfi_setgraystroke(ctx);
                break;
            case K1('g'):           /* setgray for non-stroke */
                pdfi_pop(ctx, 1);
                code = pdfi_setgrayfill(ctx);
                break;
            case K2('g','s'):       /* set graphics state from dictionary */
                pdfi_pop(ctx, 1);
                code = pdfi_setgstate(ctx, stream_dict, page_dict);
                break;
            case K1('h'):           /* closepath */
                pdfi_pop(ctx, 1);
                code = pdfi_closepath(ctx);
                break;
            case K1('i'):           /* setflat */
                pdfi_pop(ctx, 1);
                code = pdfi_setflat(ctx);
                break;
            case K2('I','D'):       /* begin inline image data */
                pdfi_pop(ctx, 1);
                code = pdfi_ID(ctx, stream_dict, page_dict, source);
                break;
            case K1('j'):           /* setlinejoin */
                pdfi_pop(ctx, 1);
                code = pdfi_setlinejoin(ctx);
                break;
            case K1('J'):           /* setlinecap */
                pdfi_pop(ctx, 1);
                code = pdfi_setlinecap(ctx);
                break;
            case K1('K'):           /* setcmyk for non-stroke */
                pdfi_pop(ctx, 1);
                code = pdfi_setcmykstroke(ctx);
                break;
            case K1('k'):           /* setcmyk for non-stroke */
                pdfi_pop(ctx, 1);
                code = pdfi_setcmykfill(ctx);
                break;
            case K1('l'):           /* lineto */
                pdfi_pop(ctx, 1);
                code = pdfi_lineto(ctx);
                break;
            case K1('m'):           /* moveto */
                pdfi_pop(ctx, 1);
                code = pdfi_moveto(ctx);
                break;
            case K1('M'):           /* setmiterlimit */
                pdfi_pop(ctx, 1);
                code = pdfi_setmiterlimit(ctx);
                break;
            case K2('M','P'):       /* define marked content point */
                pdfi_pop(ctx, 1);
                if (ctx->stack_top - ctx->stack_bot >= 1)
                    pdfi_pop(ctx, 1);
                break;
            case K1('n'):           /* newpath */
                pdfi_pop(ctx, 1);
                code = pdfi_newpath(ctx);
                break;
            case K1('q'):           /* gsave */
                pdfi_pop(ctx, 1);
                code = pdfi_gsave(ctx);
                break;
            case K1('Q'):           /* grestore */
                pdfi_pop(ctx, 1);
                code = pdfi_grestore(ctx);
                break;
            case K2('r','e'):       /* append rectangle */
                pdfi_pop(ctx, 1);
                code = pdfi_rectpath(ctx);
                break;
            case K2('R','G'):       /* set rgb colour for stroke */
                pdfi_pop(ctx, 1);
                code = pdfi_setrgbstroke(ctx);
                break;
            case K2('r','g'):       /* set rgb colour for non-stroke */
                pdfi_pop(ctx, 1);
                code = pdfi_setrgbfill(ctx);
                break;
            case K2('r','i'):       /* set rendering intent */
                pdfi_pop(ctx, 1);
                code = pdfi_ri(ctx);
                break;
            case K1('s'):           /* closepath, stroke */
                pdfi_pop(ctx, 1);
                code = pdfi_closepath_stroke(ctx);
                break;
            case K1('S'):           /* stroke */
                pdfi_pop(ctx, 1);
                code = pdfi_stroke(ctx);
                break;
            case K2('S','C'):       /* set colour for stroke */
                pdfi_pop(ctx, 1);
                code = pdfi_setstrokecolor(ctx);
                break;
            case K2('s','c'):       /* set colour for non-stroke */
                pdfi_pop(ctx, 1);
                code = pdfi_setfillcolor(ctx);
                break;
            case K3('S','C','N'):   /* set special colour for stroke */
                pdfi_pop(ctx, 1);
                code = pdfi_setstrokecolorN(ctx);
                break;
            case K3('s','c','n'):   /* set special colour for non-stroke */
                pdfi_pop(ctx, 1);
                code = pdfi_setfillcolorN(ctx);
                break;
            case K2('s','h'):       /* fill with sahding pattern */
                pdfi_pop(ctx, 1);
                code = pdfi_shading(ctx, stream_dict, page_dict);
                break;
            case K2('T','*'):       /* Move to start of next text line */
                pdfi_pop(ctx, 1);
                code = pdfi_T_star(ctx);
                break;
            case K2('T','c'):       /* set character spacing */
                pdfi_pop(ctx, 1);
                code = pdfi_Tc(ctx);
                break;
            case K2('T','d'):       /* move text position */
                pdfi_pop(ctx, 1);
                code = pdfi_Td(ctx);
                break;
            case K2('T','D'):       /* Move text position, set leading */
                pdfi_pop(ctx, 1);
                code = pdfi_TD(ctx);
                break;
            case K2('T','f'):       /* set font and size */
                pdfi_pop(ctx, 1);
                code = pdfi_Tf(ctx);
                break;
            case K2('T','j'):       /* show text */
                pdfi_pop(ctx, 1);
                code = pdfi_Tj(ctx);
                break;
            case K2('T','J'):       /* show text with individual glyph positioning */
                pdfi_pop(ctx, 1);
                code = pdfi_TJ(ctx);
                break;
            case K2('T','L'):       /* set text leading */
                pdfi_pop(ctx, 1);
                code = pdfi_TL(ctx);
                break;
            case K2('T','m'):       /* set text matrix */
                pdfi_pop(ctx, 1);
                code = pdfi_Tm(ctx);
                break;
            case K2('T','r'):       /* set text rendering mode */
                pdfi_pop(ctx, 1);
                code = pdfi_T_star(ctx);
                break;
            case K2('T','s'):       /* set text rise */
                pdfi_pop(ctx, 1);
                code = pdfi_Ts(ctx);
                break;
            case K2('T','w'):       /* set word spacing */
                pdfi_pop(ctx, 1);
                code = pdfi_Tw(ctx);
                break;
            case K2('T','z'):       /* set text matrix */
                pdfi_pop(ctx, 1);
                code = pdfi_Tz(ctx);
                break;
            case K1('v'):           /* append curve (initial point replicated) */
                pdfi_pop(ctx, 1);
                code = pdfi_v_curveto(ctx);
                break;
            case K1('w'):           /* setlinewidth */
                pdfi_pop(ctx, 1);
                code = pdfi_setlinewidth(ctx);
                break;
            case K1('W'):           /* clip */
                pdfi_pop(ctx, 1);
                code = pdfi_clip(ctx);
                break;
            case K2('W','*'):       /* eoclip */
                pdfi_pop(ctx, 1);
                code = pdfi_eoclip(ctx);
                break;
            case K1('y'):           /* append curve (final point replicated) */
                pdfi_pop(ctx, 1);
                code = pdfi_y_curveto(ctx);
                break;
            case K1('\''):          /* move to next line and show text */
                pdfi_pop(ctx, 1);
                code = pdfi_singlequote(ctx);
                break;
            case K1('"'):           /* set word and character spacing, move to next line, show text */
                pdfi_pop(ctx, 1);
                code = pdfi_doublequote(ctx);
                break;
            default:
                code = split_bogus_operator(ctx);
                if (code < 0)
                    return code;
                break;
       }
    }
    return code;
}

int pdfi_interpret_content_stream(pdf_context *ctx, pdf_dict *stream_dict, pdf_dict *page_dict)
{
    int code;
    pdf_stream *compressed_stream;
    pdf_keyword *keyword;

    code = pdfi_seek(ctx, ctx->main_stream, stream_dict->stream_offset, SEEK_SET);
    if (code < 0)
        return code;

    code = pdfi_filter(ctx, stream_dict, ctx->main_stream, &compressed_stream, false);
    if (code < 0)
        return code;

    do {
        code = pdfi_read_token(ctx, compressed_stream);
        if (code < 0) {
            if (code == gs_error_ioerror || code == gs_error_VMerror || ctx->pdfstoponerror) {
                pdfi_close_file(ctx, compressed_stream);
                return code;
            }
            continue;
        }

        if (ctx->stack_top - ctx->stack_bot <= 0) {
            if(compressed_stream->eof == true)
                break;
        }

        if (ctx->stack_top[-1]->type == PDF_KEYWORD) {
            keyword = (pdf_keyword *)ctx->stack_top[-1];

            switch(keyword->key) {
                case PDF_ENDSTREAM:
                    pdfi_close_file(ctx, compressed_stream);
                    pdfi_clearstack(ctx);
                    return 0;
                    break;
                case PDF_ENDOBJ:
                    pdfi_close_file(ctx, compressed_stream);
                    pdfi_clearstack(ctx);
                    ctx->pdf_errors |= E_PDF_MISSINGENDSTREAM;
                    if (ctx->pdfstoponerror)
                        return_error(gs_error_syntaxerror);
                    return 0;
                    break;
                case PDF_NOT_A_KEYWORD:
                    code = pdfi_interpret_stream_operator(ctx, compressed_stream, stream_dict, page_dict);
                    if (code < 0 && ctx->pdfstoponerror) {
                        pdfi_close_file(ctx, compressed_stream);
                        pdfi_clearstack(ctx);
                        return code;
                    }
                    break;
                case PDF_INVALID_KEY:
                    pdfi_clearstack(ctx);
                    break;
                default:
                    ctx->pdf_errors |= E_PDF_NOENDSTREAM;
                    pdfi_close_file(ctx, compressed_stream);
                    pdfi_clearstack(ctx);
                    return_error(gs_error_typecheck);
                    break;
            }
        }
        if(compressed_stream->eof == true)
            break;
    }while(1);

    pdfi_close_file(ctx, compressed_stream);
    return 0;
}

int pdfi_find_resource(pdf_context *ctx, unsigned char *Type, pdf_name *name, pdf_dict *stream_dict, pdf_dict *page_dict, pdf_obj **o)
{
    char Key[256];
    pdf_dict *Resources, *TypedResources;
    int code;

    *o = NULL;
    memcpy(Key, name->data, name->length);
    Key[name->length] = 0x00;

    code = pdfi_dict_get(ctx, stream_dict, "Resources", (pdf_obj **)&Resources);
    if (code == 0) {
        code = pdfi_dict_get(ctx, Resources, (const char *)Type, (pdf_obj **)&TypedResources);
        if (code == 0) {
            pdfi_countdown(Resources);
            code = pdfi_dict_get_no_store_R(ctx, TypedResources, Key, o);
            pdfi_countdown(TypedResources);
            if (code != gs_error_undefined)
                return code;
        }
    }
    code = pdfi_dict_get(ctx, page_dict, "Resources", (pdf_obj **)&Resources);
    if (code < 0)
        return code;

    code = pdfi_dict_get(ctx, Resources, (const char *)Type, (pdf_obj **)&TypedResources);
    pdfi_countdown(Resources);
    if (code < 0)
        return code;

    code = pdfi_dict_get_no_store_R(ctx, TypedResources, Key, o);
    pdfi_countdown(TypedResources);
    return code;
}