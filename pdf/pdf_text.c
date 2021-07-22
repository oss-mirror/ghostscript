/* Copyright (C) 2018-2021 Artifex Software, Inc.
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

/* Text operations for the PDF interpreter */

#include "pdf_int.h"
#include "pdf_array.h"
#include "pdf_text.h"
#include "pdf_image.h"
#include "pdf_colour.h"
#include "pdf_stack.h"
#include "pdf_gstate.h"
#include "pdf_font.h"
#include "pdf_font_types.h"
#include "pdf_trans.h"
#include "pdf_optcontent.h"

#include "gsstate.h"
#include "gsmatrix.h"
#include "gdevbbox.h"

static int pdfi_set_TL(pdf_context *ctx, double TL);

int pdfi_BT(pdf_context *ctx)
{
    int code;
    gs_matrix m;
    bool illegal_BT = false;

    if (ctx->text.BlockDepth != 0) {
        pdfi_set_warning(ctx, 0, NULL, W_PDF_NESTEDTEXTBLOCK, "pdfi_BT", NULL);
        illegal_BT = true;
    }

    gs_make_identity(&m);
    code = gs_settextmatrix(ctx->pgs, &m);
    if (code < 0)
        return code;

    code = gs_settextlinematrix(ctx->pgs, &m);
    if (code < 0)
        return code;

    /* In theory we should not perform the clip (for text rendering modes involving a clip)
     * when preserving the text rendering mode. However the pdfwrite device requires us to do
     * so. The reason is that it wraps all text operations in a sequence like 'q BT...ET Q'
     * and the grestore obviously restores away the clip before any following operation can use it.
     * The reason pdfwrite does this is historical; originally the PDF graphics state was not
     * part of the graphics library graphics state, so the only information available to
     * the device was the CTM, after the text rendering matrix and text line matrix had been
     * applied. Obviously that had to be undone at the end of the text to avoid polluting
     * the CTM for following operations.
     * Now that we track the Trm and Tlm in the graphics state it would be possible to
     * modify pdfwrite so that it emits those instead of modifying the CTM, which would avoid
     * the q/Q pair round the text, which would mean we could do away with the separate
     * clip for text rendering. However this would mean modifying both the pdfwrite device
     * and the existing PDF interpreter, which would be awkward to do. I will open an
     * enhancement bug for this, but won't begin work on it until we have completely
     * deprecated and removed the old PDF interpreter.
     * In the meantime we have to persist with this kludge.
     */
    if (gs_currenttextrenderingmode(ctx->pgs) >= 4 && ctx->text.BlockDepth == 0 /* && !ctx->device_state.preserve_tr_mode*/) {
        /* Whenever we are doing a 'clip' text rendering mode we need to
         * accumulate a path until we reach ET, and then we need to turn that
         * path into a clip and apply it (along with any existing clip). But
         * we must not disturb any existing path in the current graphics
         * state, so we need an extra gsave which we will undo when we get
         * an ET.
         */
        pdfi_gsave(ctx);
        /* Capture the current position */
        /* Start a new path (so our clip doesn't include any
         * already extant path in the graphics state)
         */
        gs_newpath(ctx->pgs);
    }

    ctx->text.initial_current_point_valid = ctx->pgs->current_point_valid;
    if (!ctx->pgs->current_point_valid)
        code = gs_moveto(ctx->pgs, 0, 0);

    ctx->text.BlockDepth++;

    if (ctx->page.has_transparency && gs_currenttextknockout(ctx->pgs) && !illegal_BT)
        gs_begin_transparency_text_group(ctx->pgs);

    return code;
}

int pdfi_ET(pdf_context *ctx)
{
    int code = 0;
    gx_clip_path *copy = NULL;

    if (ctx->text.BlockDepth == 0) {
        pdfi_set_warning(ctx, 0, NULL, W_PDF_ETNOTEXTBLOCK, "pdfi_ET", NULL);
        return_error(gs_error_syntaxerror);
    }

    ctx->text.BlockDepth--;
    /* If we have reached the end of a text block (or the outermost block
     * if we have illegally nested text blocks) and we are using a 'clip'
     * text rendering mode, then we need to apply the clip. We also need
     * to grestore back one level as we will have pushed a gsave either in
     * pdfi_BT or in pdfi_Tr. The extra gsave is so we can accumulate a
     * clipping path separately to any path already existing in the
     * graphics state.
     */

    /* See the note on text rendering modes with clip in pdfi_BT() above */
    if (ctx->text.BlockDepth == 0 && gs_currenttextrenderingmode(ctx->pgs) >= 4 /*&& !ctx->device_state.preserve_tr_mode*/) {
        gs_point initial_point;

        /* Capture the current position */
        code = gs_currentpoint(ctx->pgs, &initial_point);
        if (code >= 0) {
            gs_point adjust;

            gs_currentfilladjust(ctx->pgs, &adjust);
            code = gs_setfilladjust(ctx->pgs, (double)0.0, (double)0.0);
            if (code < 0)
                return code;

            code = gs_clip(ctx->pgs);
            if (code >= 0)
                copy = gx_cpath_alloc_shared(ctx->pgs->clip_path, ctx->memory, "save clip path");

            code = gs_setfilladjust(ctx->pgs, adjust.x, adjust.y);
            if (code < 0)
                return code;

            pdfi_grestore(ctx);
            if (copy != NULL)
                (void)gx_cpath_assign_free(ctx->pgs->clip_path, copy);
            code = gs_moveto(ctx->pgs, initial_point.x, initial_point.y);
        }
    }
    if (ctx->page.has_transparency && gs_currenttextknockout(ctx->pgs))
        gs_end_transparency_text_group(ctx->pgs);

    if (!ctx->text.initial_current_point_valid)
        gs_newpath(ctx->pgs);
    return code;
}

int pdfi_T_star(pdf_context *ctx)
{
    int code;
    gs_matrix m, mat;

    if (ctx->text.BlockDepth == 0) {
        pdfi_set_warning(ctx, 0, NULL, W_PDF_TEXTOPNOBT, "pdfi_T_star", NULL);
    }

    gs_make_identity(&m);
    m.ty += ctx->pgs->textleading;

    code = gs_matrix_multiply(&m, &ctx->pgs->textlinematrix, &mat);
    if (code < 0)
        return code;

    code = gs_settextmatrix(ctx->pgs, (gs_matrix *)&mat);
    if (code < 0)
        return code;

    code = gs_settextlinematrix(ctx->pgs, (gs_matrix *)&mat);
    return code;
}

static int pdfi_set_Tc(pdf_context *ctx, double Tc)
{
    return gs_settextspacing(ctx->pgs, Tc);
}

int pdfi_Tc(pdf_context *ctx)
{
    int code = 0;
    pdf_num *n = NULL;

    if (pdfi_count_stack(ctx) < 1) {
        pdfi_clearstack(ctx);
        return_error(gs_error_stackunderflow);
    }

    n = (pdf_num *)ctx->stack_top[-1];

    if (n->type == PDF_INT)
        code = pdfi_set_Tc(ctx, (double)n->value.i);
    else {
        if (n->type == PDF_REAL)
            code = pdfi_set_Tc(ctx, n->value.d);
        else
            code = gs_note_error(gs_error_typecheck);
    }
    pdfi_pop(ctx, 1);
    return code;
}

int pdfi_Td(pdf_context *ctx)
{
    int code;
    pdf_num *Tx = NULL, *Ty = NULL;
    gs_matrix m, mat;

    if (pdfi_count_stack(ctx) < 2) {
        pdfi_clearstack(ctx);
        return_error(gs_error_stackunderflow);
    }

    gs_make_identity(&m);

    Ty = (pdf_num *)ctx->stack_top[-1];
    Tx = (pdf_num *)ctx->stack_top[-2];

    if (Tx->type == PDF_INT) {
        m.tx = (float)Tx->value.i;
    } else {
        if (Tx->type == PDF_REAL) {
            m.tx = (float)Tx->value.d;
        } else {
            code = gs_note_error(gs_error_typecheck);
            goto Td_error;
        }
    }

    if (Ty->type == PDF_INT) {
        m.ty = (float)Ty->value.i;
    } else {
        if (Ty->type == PDF_REAL) {
            m.ty = (float)Ty->value.d;
        } else {
            code = gs_note_error(gs_error_typecheck);
            goto Td_error;
        }
    }

    if (ctx->text.BlockDepth == 0) {
        pdfi_set_warning(ctx, 0, NULL, W_PDF_TEXTOPNOBT, "pdfi_Td", NULL);

        gs_make_identity(&mat);
        code = gs_settextmatrix(ctx->pgs, &mat);
        if (code < 0)
            goto Td_error;

        code = gs_settextlinematrix(ctx->pgs, &mat);
        if (code < 0)
            goto Td_error;
    }

    code = gs_matrix_multiply(&m, &ctx->pgs->textlinematrix, &mat);
    if (code < 0)
        goto Td_error;

    code = gs_settextmatrix(ctx->pgs, (gs_matrix *)&mat);
    if (code < 0)
        goto Td_error;

    code = gs_settextlinematrix(ctx->pgs, (gs_matrix *)&mat);
    if (code < 0)
        goto Td_error;

    pdfi_pop(ctx, 2);
    return code;

Td_error:
    pdfi_pop(ctx, 2);
    return code;
}

int pdfi_TD(pdf_context *ctx)
{
    int code;
    pdf_num *Tx = NULL, *Ty = NULL;
    gs_matrix m, mat;

    if (pdfi_count_stack(ctx) < 2) {
        pdfi_clearstack(ctx);
        return_error(gs_error_stackunderflow);
    }

    gs_make_identity(&m);

    Ty = (pdf_num *)ctx->stack_top[-1];
    Tx = (pdf_num *)ctx->stack_top[-2];

    if (Tx->type == PDF_INT) {
        m.tx = (float)Tx->value.i;
    } else {
        if (Tx->type == PDF_REAL) {
            m.tx = (float)Tx->value.d;
        } else {
            code = gs_note_error(gs_error_typecheck);
            goto TD_error;
        }
    }

    if (Ty->type == PDF_INT) {
        m.ty = (float)Ty->value.i;
    } else {
        if (Ty->type == PDF_REAL) {
            m.ty = (float)Ty->value.d;
        } else {
            code = gs_note_error(gs_error_typecheck);
            goto TD_error;
        }
    }

    if (ctx->text.BlockDepth == 0) {
        pdfi_set_warning(ctx, 0, NULL, W_PDF_TEXTOPNOBT, "pdfi_TD", NULL);

        gs_make_identity(&mat);
        code = gs_settextmatrix(ctx->pgs, &mat);
        if (code < 0)
            goto TD_error;

        code = gs_settextlinematrix(ctx->pgs, &mat);
        if (code < 0)
            goto TD_error;
    }

    code = pdfi_set_TL(ctx, m.ty * 1.0f);
    if (code < 0)
        goto TD_error;

    code = gs_matrix_multiply(&m, &ctx->pgs->textlinematrix, &mat);
    if (code < 0)
        goto TD_error;

    code = gs_settextmatrix(ctx->pgs, (gs_matrix *)&mat);
    if (code < 0)
        goto TD_error;

    code = gs_settextlinematrix(ctx->pgs, (gs_matrix *)&mat);
    if (code < 0)
        goto TD_error;

    pdfi_pop(ctx, 2);
    return code;

TD_error:
    pdfi_pop(ctx, 2);
    return code;
}

/* This routine sets up most of the text params structure. In particular it
 * creates and initialises the x and y widths arrays, and for type 3 fonts
 * it creates and populates the 'chars' member.
 * It also sets the delta_sace member and partially set sup the 'operation'
 * bitfield. It does not set any of the TEXT_DO_* fields because we intend
 * to use this routine to set up the 'common' parts of the structure and
 * then we will twiddle the 'TEXT_DO_*' fields as required for the type of
 * operation we are doing (fill, create path for stroke, create path for fill)
 */
static int pdfi_show_set_params(pdf_context *ctx, pdf_string *s, gs_text_params_t *text)
{
    pdf_font *current_font = NULL;
    gs_matrix mat;
    float *x_widths = NULL, *y_widths = NULL, width;
    double Tw = 0, Tc = 0;
    int i, code;

    text->data.chars = NULL;
    text->x_widths = NULL;
    text->y_widths = NULL;

    /* NOTE: we don't scale the FontMatrix we leave it as the default
     * and do all our scaling with the textmatrix/ctm. This saves having
     * to create multiple instances of the same font, and simplifies
     * composite fonts significantly.
     */
    current_font = pdfi_get_current_pdf_font(ctx);

    if (current_font == NULL)
        return_error(gs_error_invalidfont);

    /* Division by PDFfontsize because these are in unscaled font units,
       and the font scale is now pickled into the text matrix, so we have to
       undo that.
     */
    Tc = gs_currenttextspacing(ctx->pgs) / ctx->pgs->PDFfontsize;

    if (current_font->pdfi_font_type == e_pdf_font_type1 ||
        current_font->pdfi_font_type == e_pdf_font_cff ||
        current_font->pdfi_font_type == e_pdf_font_type3 ||
        current_font->pdfi_font_type == e_pdf_font_cff ||
        current_font->pdfi_font_type == e_pdf_font_truetype ||
        current_font->pdfi_font_type == e_pdf_font_type0)
    {
        /* For Type 0 fonts, we apply the DW/W/DW2/W2 values when we retrieve the metrics for
           setcachedevice - see pdfi_fapi_set_cache()
         */
        if (current_font->pdfi_font_type == e_pdf_font_type0 || current_font->Widths == NULL) {
            text->operation = TEXT_RETURN_WIDTH;
            if (Tc != 0) {
                text->operation |= TEXT_ADD_TO_ALL_WIDTHS;
                if (current_font->pfont && current_font->pfont->WMode == 0) {
                    text->delta_all.x = Tc;
                    text->delta_all.y = 0;
                } else {
                    text->delta_all.y = Tc;
                    text->delta_all.x = 0;
                }
            }
        } else {
            gs_point pt;

            x_widths = (float *)gs_alloc_bytes(ctx->memory, s->length * sizeof(float), "X widths array for text");
            y_widths = (float *)gs_alloc_bytes(ctx->memory, s->length * sizeof(float), "Y widths array for text");
            if (x_widths == NULL || y_widths == NULL) {
                code = gs_note_error(gs_error_VMerror);
                goto text_params_error;
            }

            memset(x_widths, 0x00, s->length * sizeof(float));
            memset(y_widths, 0x00, s->length * sizeof(float));

            /* To calculate the Width (which is defined in unscaled text units) as a value in user
             * space we need to transform it by the FontMatrix
             */
            mat = current_font->pfont->FontMatrix;

            for (i = 0;i < s->length; i++) {
                /* Get the width (in unscaled text units) */
                if (s->data[i] < current_font->FirstChar || s->data[i] > current_font->LastChar)
                    width = 0;
                else
                    width = current_font->Widths[s->data[i] - current_font->FirstChar];
                /* And convert the width into an appropriate value for the current environment */
                gs_distance_transform(width, 0, &mat, &pt);
                x_widths[i] = hypot(pt.x, pt.y) * (width < 0 ? -1.0 : 1.0);
                /* Add any Tc value */
                x_widths[i] += Tc;
            }
            text->operation = TEXT_RETURN_WIDTH | TEXT_REPLACE_WIDTHS;
            text->x_widths = x_widths;
            text->y_widths = y_widths;
            text->widths_size = s->length * 2;
        }

        Tw = gs_currentwordspacing(ctx->pgs);
        if (Tw != 0) {
            text->operation |= TEXT_ADD_TO_SPACE_WIDTH;
            /* Division by PDFfontsize because these are in unscaled font units,
               and the font scale is now pickled into the text matrix, so we have to
               undo that.
             */
            if (current_font->pfont && current_font->pfont->WMode == 0) {
                text->delta_space.x = Tw / ctx->pgs->PDFfontsize;
                text->delta_space.y = 0;
            } else {
                text->delta_space.y = Tw / ctx->pgs->PDFfontsize;
                text->delta_space.x = 0;
            }
            text->space.s_char = 0x20;
        }

        if (current_font->pdfi_font_type == e_pdf_font_type3) {
            text->operation |= TEXT_FROM_CHARS;
            text->data.chars = (const gs_char *)gs_alloc_bytes(ctx->memory, s->length * sizeof(gs_char), "string gs_chars");
            if (!text->data.chars) {
                code = gs_note_error(gs_error_VMerror);
                goto text_params_error;
            }

            for (i = 0; i < s->length; i++) {
                ((gs_char *)text->data.chars)[i] = (gs_char)s->data[i];
            }
        }
        else {
            text->operation |= TEXT_FROM_BYTES;
            text->data.bytes = (const byte *)s->data;
        }
        text->size = s->length;
    }
    return 0;

text_params_error:
    gs_free_object(ctx->memory, x_widths, "X widths array for text");
    text->x_widths = NULL;
    gs_free_object(ctx->memory, y_widths, "Y widths array for text");
    text->y_widths = NULL;
    gs_free_object(ctx->memory, (void *)text->data.chars, "string gs_chars");
    text->data.chars = NULL;
    return code;
}

/* These routines actually perform the fill/stroke/clip of the text.
 * We build up the compound cases by reusing the basic cases which
 * is why we reset the TEXT_DO_ fields after use.
 */

/* Mode 0 - fill */
static int pdfi_show_Tr_0(pdf_context *ctx, gs_text_params_t *text)
{
    int code;
    gs_text_enum_t *penum=NULL, *saved_penum=NULL;

    /* just draw the text */
    text->operation |= TEXT_DO_DRAW;

    code = gs_text_begin(ctx->pgs, text, ctx->memory, &penum);
    if (code >= 0) {
        penum->single_byte_space = true;
        saved_penum = ctx->text.current_enum;
        ctx->text.current_enum = penum;
        code = gs_text_process(penum);
        gs_text_release(ctx->pgs, penum, "pdfi_Tj");
        ctx->text.current_enum = saved_penum;
    }
    text->operation &= ~TEXT_DO_DRAW;
    return code;
}

/* Mode 1 - stroke */
static int pdfi_show_Tr_1(pdf_context *ctx, gs_text_params_t *text)
{
    int code;
    gs_text_enum_t *penum=NULL, *saved_penum=NULL;
    gs_point end_point, initial_point;

    end_point.x = end_point.y = initial_point.x = initial_point.y = 0;

    /* Capture the current position */
    code = gs_currentpoint(ctx->pgs, &initial_point);
    if (code < 0)
        return code;

    /* We don't want to disturb the current path, so do a gsave now
     * We will grestore back to this point after we have stroked the
     * text, which will leave any current path unchanged.
     */
    pdfi_gsave(ctx);

    /* Start a new path (so our stroke doesn't include any
     * already extant path in the graphics state)
     */
    code = gs_newpath(ctx->pgs);
    if (code < 0)
        goto Tr1_error;
    code = gs_moveto(ctx->pgs, initial_point.x, initial_point.y);
    if (code < 0)
        goto Tr1_error;

    /* Don't draw the text, create a path suitable for stroking */
    text->operation |= TEXT_DO_FALSE_CHARPATH;

    /* Run the text methods to create the path */
    code = gs_text_begin(ctx->pgs, text, ctx->memory, &penum);
    if (code < 0)
        goto Tr1_error;

    penum->single_byte_space = true;
    saved_penum = ctx->text.current_enum;
    ctx->text.current_enum = penum;
    code = gs_text_process(penum);
    gs_text_release(ctx->pgs, penum, "pdfi_Tj");
    ctx->text.current_enum = saved_penum;
    if (code < 0)
        goto Tr1_error;

    /* After a stroke operation there is no current point and we need
     * it to be set as if we had drawn the text. So capture it now
     * and we will append a 'move' to the current point when we have
     * finished the stroke.
     */
    code = gs_currentpoint(ctx->pgs, &end_point);
    if (code < 0)
        goto Tr1_error;
    /* Change to the current stroking colour */
    gs_swapcolors_quick(ctx->pgs);
    /* Finally, stroke the actual path */
    code = gs_stroke(ctx->pgs);
    /* Switch back to the non-stroke colour */
    gs_swapcolors_quick(ctx->pgs);

Tr1_error:
    /* And grestore back to where we started */
    pdfi_grestore(ctx);
    /* If everything went well, then move the current point to the
     * position we captured at the end of the path creation */
    if (code >= 0)
        code = gs_moveto(ctx->pgs, end_point.x, end_point.y);

    text->operation &= ~TEXT_DO_FALSE_CHARPATH;
    return code;
}

/* Mode 2 - fill then stroke */
static int pdfi_show_Tr_2(pdf_context *ctx, gs_text_params_t *text)
{
    int code, restart = 0;
    gs_text_enum_t *penum=NULL, *saved_penum=NULL;
    gs_point end_point, initial_point;

    end_point.x = end_point.y = initial_point.x = initial_point.y = 0;

    /* Capture the current position */
    code = gs_currentpoint(ctx->pgs, &initial_point);
    if (code < 0)
        return code;

    /* We don't want to disturb the current path, so do a gsave now
     * We will grestore back to this point after we have stroked the
     * text, which will leave any current path unchanged.
     */
    pdfi_gsave(ctx);

    /* Start a new path (so our stroke doesn't include any
     * already extant path in the graphics state)
     */
    code = gs_newpath(ctx->pgs);
    if (code < 0)
        goto Tr1_error;
    code = gs_moveto(ctx->pgs, initial_point.x, initial_point.y);
    if (code < 0)
        goto Tr1_error;

    /* Don't draw the text, create a path suitable for stroking */
    text->operation |= TEXT_DO_FALSE_CHARPATH;

    /* Run the text methods to create the path */
    code = gs_text_begin(ctx->pgs, text, ctx->memory, &penum);
    if (code < 0)
        goto Tr1_error;

    penum->single_byte_space = true;
    saved_penum = ctx->text.current_enum;
    ctx->text.current_enum = penum;
    code = gs_text_process(penum);
    gs_text_release(ctx->pgs, penum, "pdfi_Tj");
    ctx->text.current_enum = saved_penum;
    if (code < 0)
        goto Tr1_error;

    /* After a stroke operation there is no current point and we need
     * it to be set as if we had drawn the text. So capture it now
     * and we will append a 'move' to the current point when we have
     * finished the stroke.
     */
    code = gs_currentpoint(ctx->pgs, &end_point);
    if (code < 0)
        goto Tr1_error;
    code = gs_fillstroke(ctx->pgs, &restart);

Tr1_error:
    /* And grestore back to where we started */
    pdfi_grestore(ctx);
    /* If everything went well, then move the current point to the
     * position we captured at the end of the path creation */
    if (code >= 0)
        code = gs_moveto(ctx->pgs, end_point.x, end_point.y);

    text->operation &= ~TEXT_DO_FALSE_CHARPATH;
    return code;
}

static int pdfi_show_Tr_3(pdf_context *ctx, gs_text_params_t *text)
{
    int code;
    gs_text_enum_t *penum=NULL, *saved_penum=NULL;

    /* Don't draw the text */
    text->operation |= TEXT_DO_NONE | TEXT_RENDER_MODE_3;

    /* Run the text methods to create the path */
    code = gs_text_begin(ctx->pgs, text, ctx->memory, &penum);
    if (code < 0)
        return code;

    penum->single_byte_space = true;
    saved_penum = ctx->text.current_enum;
    ctx->text.current_enum = penum;
    code = gs_text_process(penum);
    gs_text_release(ctx->pgs, penum, "pdfi_Tj");
    ctx->text.current_enum = saved_penum;

    return code;
}

/* Prototype the basic 'clip' function, as the following routines will all use it */
static int pdfi_show_Tr_7(pdf_context *ctx, gs_text_params_t *text);

static int pdfi_show_Tr_4(pdf_context *ctx, gs_text_params_t *text)
{
    int code;
    gs_point initial_point;

    /* Capture the current position */
    code = gs_currentpoint(ctx->pgs, &initial_point);
    if (code < 0)
        return code;

    /* First fill the text */
    code = pdfi_show_Tr_0(ctx, text);
    if (code < 0)
        return code;

    /* Return the current point to the initial point */
    code = gs_moveto(ctx->pgs, initial_point.x, initial_point.y);
    if (code >= 0)
        /* And add the text to the acumulated path for clipping */
        code = pdfi_show_Tr_7(ctx, text);

    return code;
}

static int pdfi_show_Tr_5(pdf_context *ctx, gs_text_params_t *text)
{
    int code;
    gs_point initial_point;

    /* Capture the current position */
    code = gs_currentpoint(ctx->pgs, &initial_point);
    if (code < 0)
        return code;

    /* First stroke the text */
    code = pdfi_show_Tr_1(ctx, text);
    if (code < 0)
        return code;

    /* Return the current point to the initial point */
    code = gs_moveto(ctx->pgs, initial_point.x, initial_point.y);
    if (code >= 0)
        /* And add the text to the acumulated path for clipping */
        code = pdfi_show_Tr_7(ctx, text);

    return code;
}

static int pdfi_show_Tr_6(pdf_context *ctx, gs_text_params_t *text)
{
    int code;
    gs_point initial_point;

    /* Capture the current position */
    code = gs_currentpoint(ctx->pgs, &initial_point);
    if (code < 0)
        return code;

    /* First fill and stroke the text */
    code = pdfi_show_Tr_2(ctx, text);
    if (code < 0)
        return code;

    /* Return the current point to the initial point */
    code = gs_moveto(ctx->pgs, initial_point.x, initial_point.y);
    if (code >= 0)
        /* And add the text to the acumulated path for clipping */
        code = pdfi_show_Tr_7(ctx, text);

    return code;
}

static int pdfi_show_Tr_7(pdf_context *ctx, gs_text_params_t *text)
{
    int code;
    gs_text_enum_t *penum=NULL, *saved_penum=NULL;

    /* Don't draw the text, create a path suitable for filling */
    text->operation |= TEXT_DO_TRUE_CHARPATH;

    /* Run the text methods to create the path */
    code = gs_text_begin(ctx->pgs, text, ctx->memory, &penum);
    if (code < 0)
        goto Tr7_error;

    penum->single_byte_space = true;
    saved_penum = ctx->text.current_enum;
    ctx->text.current_enum = penum;
    code = gs_text_process(penum);
    gs_text_release(ctx->pgs, penum, "pdfi_Tj");
    ctx->text.current_enum = saved_penum;

Tr7_error:
    text->operation &= ~TEXT_DO_TRUE_CHARPATH;
    return code;
}

static int pdfi_show_Tr_preserve(pdf_context *ctx, gs_text_params_t *text)
{
    int Trmode = 0, code;
    pdf_font *current_font = NULL;
    gs_point initial_point;

    current_font = pdfi_get_current_pdf_font(ctx);
    if (current_font == NULL)
        return_error(gs_error_invalidfont);

    /* Capture the current position, in case we need it for clipping */
    code = gs_currentpoint(ctx->pgs, &initial_point);

    Trmode = gs_currenttextrenderingmode(ctx->pgs);
    if (Trmode == 3) {
        if (current_font->pdfi_font_type == e_pdf_font_type3)
            text->operation = TEXT_FROM_CHARS | TEXT_DO_NONE | TEXT_RENDER_MODE_3;
        else
            text->operation = TEXT_FROM_BYTES | TEXT_DO_NONE | TEXT_RENDER_MODE_3;
    }

    code = pdfi_show_Tr_0(ctx, text);
    if (code < 0)
        return code;

    /* See the comment in pdfi_BT() aboe regarding  text rendering modes and clipping.
     * NB regardless of the device, we never apply clipping modes to text in a type 3 font.
     */
    if (Trmode >= 4 && current_font->pdfi_font_type != e_pdf_font_type3) {
        text->operation &= ~TEXT_DO_DRAW;

        gs_moveto(ctx->pgs, initial_point.x, initial_point.y);
        code = pdfi_show_Tr_7(ctx, text);
    }
    return code;
}

static int pdfi_show(pdf_context *ctx, pdf_string *s)
{
    int code = 0;
    int code1 = 0;
    gs_text_params_t text;
    pdf_font *current_font = NULL;
    int Trmode = 0;
    int initial_gsave_level = ctx->pgs->level;
    pdfi_trans_state_t state;

    if (ctx->text.BlockDepth == 0) {
        pdfi_set_warning(ctx, 0, NULL, W_PDF_TEXTOPNOBT, "pdfi_show", NULL);
    }

    if (hypot(ctx->pgs->ctm.xx, ctx->pgs->ctm.xy) == 0.0
     || hypot(ctx->pgs->ctm.yy, ctx->pgs->ctm.yx) == 0.0) {
        /* This can mean a font scaled to 0, which appears to be valid, or a degenerate
         * ctm/text matrix, which isn't. A degenrate matrix will have triggered a warning
         * before, so we just skip the operation here.
         */
         return 0;
    }

    current_font = pdfi_get_current_pdf_font(ctx);
    if (current_font == NULL)
        return_error(gs_error_invalidfont);

    code = pdfi_show_set_params(ctx, s, &text);
    if (code < 0)
        goto show_error;

    code = pdfi_trans_setup_text(ctx, &state, true);
    if (code >= 0) {
        if (ctx->device_state.preserve_tr_mode) {
        code = pdfi_show_Tr_preserve(ctx, &text);
        } else {
            Trmode = gs_currenttextrenderingmode(ctx->pgs);

            /* The spec says that we ignore text rendering modes other than 0 for
             * type 3 fonts, but Acrobat also honours mode 3 (do nothing)
             */
            if (current_font->pdfi_font_type == e_pdf_font_type3 && Trmode != 0 && Trmode != 3)
                Trmode = 0;

            switch(Trmode) {
            case 0:
                code = pdfi_show_Tr_0(ctx, &text);
                break;
            case 1:
                code = pdfi_show_Tr_1(ctx, &text);
                break;
            case 2:
                code = pdfi_show_Tr_2(ctx, &text);
                break;
            case 3:
                code = pdfi_show_Tr_3(ctx, &text);
                break;
            case 4:
                code = pdfi_show_Tr_4(ctx, &text);
                break;
            case 5:
                code = pdfi_show_Tr_5(ctx, &text);
                break;
            case 6:
                code = pdfi_show_Tr_6(ctx, &text);
                break;
            case 7:
                code = pdfi_show_Tr_7(ctx, &text);
                break;
            default:
                break;
            }
        }
        code1 = pdfi_trans_teardown_text(ctx, &state);
        if (code == 0)
            code = code1;
    }

    /* We shouldn't need to do this, but..... It turns out that if we have text rendering mode 3 set
     * then gs_text_begin will execute a gsave, push the nulldevice and alter the saved gsave level.
     * If we then get an error while processing the text we don't gs_restore enough times which
     * leaves the nulldevice as the current device, and eats all the following content!
     * To avoid that, we save the gsave depth at the start of this routine, and grestore enough
     * times to get back to the same level. I think this is actually a bug in the graphics library
     * but it doesn't get exposed in normal usage so we'll just work around it here.
     */
    while(ctx->pgs->level > initial_gsave_level)
        gs_grestore(ctx->pgs);

show_error:
    if ((void *)text.data.chars != (void *)s->data)
        gs_free_object(ctx->memory, (void *)text.data.chars, "string gs_chars");

    gs_free_object(ctx->memory, (void *)text.x_widths, "Free X widths array on error");
    gs_free_object(ctx->memory, (void *)text.y_widths, "Free Y widths array on error");
    return code;
}

/* NOTE: the bounding box this generates has llx and lly at 0,0,
   so is arguably not a "real" bounding box
 */
int pdfi_string_bbox(pdf_context *ctx, pdf_string *s, gs_rect *bboxout, gs_point *advance_width, bool for_stroke)
{
    int code = 0;
    gx_device_bbox *bbdev;
    pdf_font *current_font = pdfi_get_current_pdf_font(ctx);
    gs_matrix tmpmat, Trm, Trm_ctm;
    gs_point cppt, startpt;

    if (current_font == NULL)
        return_error(gs_error_invalidfont);


    for_stroke = current_font->pdfi_font_type == e_pdf_font_type3 ? false : for_stroke;

    bbdev = gs_alloc_struct_immovable(ctx->memory, gx_device_bbox, &st_device_bbox, "pdfi_string_bbox(bbdev)");
    if (bbdev == NULL)
        return_error(gs_error_VMerror);

    gx_device_bbox_init(bbdev, NULL, ctx->memory);
    gx_device_retain((gx_device *)bbdev, true);
    gx_device_bbox_set_white_opaque(bbdev, true);

    code = pdfi_gsave(ctx);
    if (code < 0) {
        gx_device_retain((gx_device *)bbdev, false);
        return code;
    }
    /* The bbox device needs a fairly high resolution to get accurate results */
    gx_device_set_resolution((gx_device *)bbdev, 720.0, 720.0);

    code = gs_setdevice_no_erase(ctx->pgs, (gx_device *)bbdev);
    if (code < 0)
        goto out;

    Trm.xx = ctx->pgs->PDFfontsize * (ctx->pgs->texthscaling / 100);
    Trm.xy = 0;
    Trm.yx = 0;
    Trm.yy = ctx->pgs->PDFfontsize;
    Trm.tx = 0;
    Trm.ty = ctx->pgs->textrise;

    memcpy(&tmpmat, &ctx->pgs->textmatrix, sizeof(tmpmat));
    /* We want to avoid any translations unrelated to the extents of the text */
    tmpmat.tx = tmpmat.ty = 0;
    gs_matrix_multiply(&Trm, &tmpmat, &Trm);

    memcpy(&tmpmat, &ctm_only(ctx->pgs), sizeof(tmpmat));
    /* As above */
    tmpmat.tx = tmpmat.ty = 0;
    gs_matrix_multiply(&Trm, &tmpmat, &Trm_ctm);
    gs_setmatrix(ctx->pgs, &Trm_ctm);

    gs_settextrenderingmode(ctx->pgs, for_stroke ? 2 : 0);

    code = pdfi_gs_setgray(ctx, 1.0);
    if (code < 0)
        goto out;

    /* The bbox device (not surprisingly) clips to the device width/height
       so we have to offset our initial point to cope with glyphs that include
       negative coordinates. Most significantly, descender features - hence the
       larger y offset.
       The offsets are guesses.
     */
    startpt.x = ctx->pgs->PDFfontsize;
    startpt.y = ctx->pgs->PDFfontsize * 16.0 * (ctx->pgs->textrise >= 0 ? 1 : -ctx->pgs->textrise);
    code = gs_moveto(ctx->pgs, startpt.x, startpt.y);
    if (code < 0)
        goto out;

    code = pdfi_show(ctx, s);
    if (code < 0)
        goto out;

    code = gx_device_bbox_bbox(bbdev, bboxout);
    if (code < 0)
        goto out;

    bboxout->q.x -= bboxout->p.x;
    bboxout->q.y -= bboxout->p.y;
    bboxout->p.x = bboxout->p.y = 0;

    code = gs_currentpoint(ctx->pgs, &cppt);
    if (code >= 0) {
        code = gs_point_transform(startpt.x, startpt.y, &ctm_only(ctx->pgs), &startpt);
        if (code < 0)
            goto out;
        advance_width->x = ctx->pgs->current_point.x - startpt.x;
        advance_width->y = ctx->pgs->current_point.y - startpt.y;
        code = gs_point_transform_inverse(advance_width->x, advance_width->y, &tmpmat, advance_width);
    }
out:
    pdfi_grestore(ctx);
    gx_device_retain((gx_device *)bbdev, false);

    return code;
}

int pdfi_Tj(pdf_context *ctx)
{
    int code = 0;
    pdf_string *s = NULL;
    gs_matrix saved, Trm, *matrix, preserve;
    gs_point initial_point, current_point, pt;
    double linewidth = ctx->pgs->line_params.half_width;

    if (pdfi_count_stack(ctx) < 1)
        return_error(gs_error_stackunderflow);

    if (pdfi_oc_is_off(ctx))
        goto exit;

    s = (pdf_string *)ctx->stack_top[-1];
    if (s->type != PDF_STRING)
        return_error(gs_error_typecheck);

    /* Save the CTM for later restoration */
    saved = ctm_only(ctx->pgs);
    gs_currentpoint(ctx->pgs, &initial_point);

    Trm.xx = ctx->pgs->PDFfontsize * (ctx->pgs->texthscaling / 100);
    Trm.xy = 0;
    Trm.yx = 0;
    Trm.yy = ctx->pgs->PDFfontsize;
    Trm.tx = 0;
    Trm.ty = ctx->pgs->textrise;

    gs_matrix_multiply(&Trm, &ctx->pgs->textmatrix, &Trm);

    if (!ctx->device_state.preserve_tr_mode)
        matrix = &Trm;
    else {
        /* We have to adjust the stroke width for pdfwrite so that we take into
         * account the CTM, but we do not spply the font scaling. Because of
         * the disconnect between pdfwrite and the interpreter, we also have to
         * remove the scaling due ot the resolution.
         */
        gs_matrix devmatrix;
        gx_device *device = gs_currentdevice(ctx->pgs);

        devmatrix.xx = 72.0 / device->HWResolution[0];
        devmatrix.xy = 0;
        devmatrix.yx = 0;
        devmatrix.yy = 72.0 / device->HWResolution[1];
        devmatrix.tx = 0;
        devmatrix.ty = 0;

        code = gs_matrix_multiply(&saved, &devmatrix, &preserve);
        if (code < 0)
            goto exit;

        matrix = &preserve;
    }

    gs_distance_transform(ctx->pgs->line_params.half_width, 0, matrix, &pt);
    ctx->pgs->line_params.half_width = sqrt((pt.x * pt.x) + (pt.y * pt.y));

    gs_matrix_multiply(&Trm, &ctm_only(ctx->pgs), &Trm);
    gs_setmatrix(ctx->pgs, &Trm);

    code = gs_moveto(ctx->pgs, 0, 0);
    if (code < 0)
        goto Tj_error;

    code = pdfi_show(ctx, s);

    ctx->pgs->line_params.half_width = linewidth;
    /* Update the Text matrix with the current point, for the next operation
     */
    gs_currentpoint(ctx->pgs, &current_point);
    Trm.xx = ctx->pgs->PDFfontsize * (ctx->pgs->texthscaling / 100);
    Trm.xy = 0;
    Trm.yx = 0;
    Trm.yy = ctx->pgs->PDFfontsize;
    Trm.tx = 0;
    Trm.ty = 0;
    gs_matrix_multiply(&Trm, &ctx->pgs->textmatrix, &Trm);

    gs_distance_transform(current_point.x, current_point.y, &Trm, &pt);
    ctx->pgs->textmatrix.tx += pt.x;
    ctx->pgs->textmatrix.ty += pt.y;

Tj_error:
    /* Restore the CTM to the saved value */
    gs_setmatrix(ctx->pgs, &saved);
    /* And restore the currentpoint */
    gs_moveto(ctx->pgs, initial_point.x, initial_point.y);
    /* And the line width */
    ctx->pgs->line_params.half_width = linewidth;

 exit:
    pdfi_pop(ctx, 1);
    return code;
}

int pdfi_TJ(pdf_context *ctx)
{
    int code = 0, i;
    pdf_array *a = NULL;
    pdf_obj *o = NULL;
    double dx = 0;
    gs_point pt;
    gs_matrix saved, Trm;
    gs_point initial_point, current_point;
    double linewidth = ctx->pgs->line_params.half_width;
    pdf_font *current_font = NULL;

    current_font = pdfi_get_current_pdf_font(ctx);
    if (current_font == NULL)
        return_error(gs_error_invalidfont);

    if (ctx->text.BlockDepth == 0) {
        pdfi_set_warning(ctx, 0, NULL, W_PDF_TEXTOPNOBT, "pdfi_TJ", NULL);
    }

    if (pdfi_count_stack(ctx) < 1)
        return_error(gs_error_stackunderflow);

    if (pdfi_oc_is_off(ctx))
        goto exit;

    a = (pdf_array *)ctx->stack_top[-1];
    if (a->type != PDF_ARRAY) {
        pdfi_pop(ctx, 1);
        return gs_note_error(gs_error_typecheck);
    }

    /* Save the CTM for later restoration */
    saved = ctm_only(ctx->pgs);
    gs_currentpoint(ctx->pgs, &initial_point);

    /* Calculate the text rendering matrix, see section 1.7 PDF Reference
     * page 409, section 5.3.3 Text Space details.
     */
    Trm.xx = ctx->pgs->PDFfontsize * (ctx->pgs->texthscaling / 100);
    Trm.xy = 0;
    Trm.yx = 0;
    Trm.yy = ctx->pgs->PDFfontsize;
    Trm.tx = 0;
    Trm.ty = ctx->pgs->textrise;

    gs_matrix_multiply(&Trm, &ctx->pgs->textmatrix, &Trm);

    gs_distance_transform_inverse(ctx->pgs->line_params.half_width, 0, &Trm, &pt);
    ctx->pgs->line_params.half_width = sqrt((pt.x * pt.x) + (pt.y * pt.y));

    gs_matrix_multiply(&Trm, &ctm_only(ctx->pgs), &Trm);
    gs_setmatrix(ctx->pgs, &Trm);

    code = gs_moveto(ctx->pgs, 0, 0);
    if (code < 0)
        goto TJ_error;

    for (i = 0; i < pdfi_array_size(a); i++) {
        code = pdfi_array_get(ctx, a, (uint64_t)i, &o);
        if (code < 0)
            goto TJ_error;

        if (o->type == PDF_INT) {
            dx = (double)((pdf_num *)o)->value.i / -1000;
            if (current_font->pfont && current_font->pfont->WMode == 0)
                code = gs_rmoveto(ctx->pgs, dx, 0);
            else
                code = gs_rmoveto(ctx->pgs, 0, dx);
            if (code < 0)
                goto TJ_error;
        } else {
            if (o->type == PDF_REAL) {
                dx = ((pdf_num *)o)->value.d / -1000;
                if (current_font->pfont && current_font->pfont->WMode == 0)
                    code = gs_rmoveto(ctx->pgs, dx, 0);
                else
                    code = gs_rmoveto(ctx->pgs, 0, dx);
                if (code < 0)
                    goto TJ_error;
            } else {
                if (o->type == PDF_STRING)
                    code = pdfi_show(ctx, (pdf_string *)o);
                else
                    code = gs_note_error(gs_error_typecheck);
            }
        }
        pdfi_countdown(o);
        o = NULL;
        if (code < 0)
            goto TJ_error;
    }

    /* Update the Text matrix with the current point, for the next operation
     */
    gs_currentpoint(ctx->pgs, &current_point);
    Trm.xx = ctx->pgs->PDFfontsize * (ctx->pgs->texthscaling / 100);
    Trm.xy = 0;
    Trm.yx = 0;
    Trm.yy = ctx->pgs->PDFfontsize;
    Trm.tx = 0;
    Trm.ty = 0;
    gs_matrix_multiply(&Trm, &ctx->pgs->textmatrix, &Trm);

    gs_distance_transform(current_point.x, current_point.y, &Trm, &pt);
    ctx->pgs->textmatrix.tx += pt.x;
    ctx->pgs->textmatrix.ty += pt.y;

TJ_error:
    pdfi_countdown(o);
    /* Restore the CTM to the saved value */
    gs_setmatrix(ctx->pgs, &saved);
    /* And restore the currentpoint */
    gs_moveto(ctx->pgs, initial_point.x, initial_point.y);
    /* And the line width */
    ctx->pgs->line_params.half_width = linewidth;

 exit:
    pdfi_pop(ctx, 1);
    return code;
}

static int pdfi_set_TL(pdf_context *ctx, double TL)
{
    return gs_settextleading(ctx->pgs, TL);
}

int pdfi_TL(pdf_context *ctx)
{
    int code = 0;
    pdf_num *n = NULL;

    if (pdfi_count_stack(ctx) < 1) {
        pdfi_clearstack(ctx);
        return_error(gs_error_stackunderflow);
    }

    n = (pdf_num *)ctx->stack_top[-1];

    if (n->type == PDF_INT)
        code = pdfi_set_TL(ctx, (double)(n->value.i * -1));
    else {
        if (n->type == PDF_REAL)
            code = pdfi_set_TL(ctx, n->value.d * -1.0);
        else
            code = gs_note_error(gs_error_typecheck);
    }
    pdfi_pop(ctx, 1);
    return code;
}

int pdfi_Tm(pdf_context *ctx)
{
    int code = 0, i;
    float m[6];
    pdf_num *n = NULL;
    gs_matrix mat;

    if (pdfi_count_stack(ctx) < 6) {
        pdfi_clearstack(ctx);
        return_error(gs_error_stackunderflow);
    }
    for (i = 1;i < 7;i++) {
        n = (pdf_num *)ctx->stack_top[-1 * i];
        if (n->type == PDF_INT)
            m[6 - i] = (float)n->value.i;
        else {
            if (n->type == PDF_REAL)
                m[6 - i] = (float)n->value.d;
            else {
                pdfi_pop(ctx, 6);
                return_error(gs_error_typecheck);
            }
        }
    }
    pdfi_pop(ctx, 6);

    if (ctx->text.BlockDepth == 0) {
        pdfi_set_warning(ctx, 0, NULL, W_PDF_TEXTOPNOBT, "pdfi_Tm", NULL);

        gs_make_identity(&mat);
        code = gs_settextmatrix(ctx->pgs, &mat);
        if (code < 0)
            return code;

        code = gs_settextlinematrix(ctx->pgs, &mat);
        if (code < 0)
            return code;
    }

    if (hypot(m[0], m[1]) == 0.0 || hypot(m[3], m[2]) == 0.0)
        pdfi_set_warning(ctx, 0, NULL, W_PDF_DEGENERATETM, "pdfi_Tm", NULL);

    code = gs_settextmatrix(ctx->pgs, (gs_matrix *)&m);
    if (code < 0)
        return code;

    code = gs_settextlinematrix(ctx->pgs, (gs_matrix *)&m);

    return code;
}

int pdfi_Tr(pdf_context *ctx)
{
    int code = 0, mode = 0;
    pdf_num *n = NULL;

    if (pdfi_count_stack(ctx) < 1) {
        pdfi_clearstack(ctx);
        return_error(gs_error_stackunderflow);
    }

    n = (pdf_num *)ctx->stack_top[-1];

    if (n->type == PDF_INT)
        mode = n->value.i;
    else {
        if (n->type == PDF_REAL)
            mode = (int)n->value.d;
        else {
            pdfi_pop(ctx, 1);
            return_error(gs_error_typecheck);
        }
    }
    pdfi_pop(ctx, 1);

    if (mode < 0 || mode > 7)
        code = gs_note_error(gs_error_rangecheck);
    else {
/* See comment regarding text rendering modes involving clip in pdfi_BT() above.
 * The commented out code here will be needed when we enhance pdfwrite so that
 * we don't need to do the clip separately.
 */
/*        if (!ctx->device_state.preserve_tr_mode) {*/
            gs_point initial_point;

            /* Detect attempts to switch from a clipping mode to a non-clipping
             * mode, this is defined as invalid in the spec.
             */
            if (gs_currenttextrenderingmode(ctx->pgs) > 3 && mode < 4 && ctx->text.BlockDepth != 0)
                pdfi_set_warning(ctx, 0, NULL, W_PDF_BADTRSWITCH, "pdfi_Tr", NULL);

            if (gs_currenttextrenderingmode(ctx->pgs) < 4 && mode >= 4 && ctx->text.BlockDepth != 0) {
                /* If we are switching from a non-clip text rendering mode to a
                 * mode involving a cip, and we are already inside a text block,
                 * put a gsave in place so that we can accumulate a path for
                 * clipping without disturbing any existing path in the
                 * graphics state.
                 */
                gs_settextrenderingmode(ctx->pgs, mode);
                pdfi_gsave(ctx);
                /* Capture the current position */
                code = gs_currentpoint(ctx->pgs, &initial_point);
                /* Start a new path (so our clip doesn't include any
                 * already extant path in the graphics state)
                 */
                gs_newpath(ctx->pgs);
                gs_moveto(ctx->pgs, initial_point.x, initial_point.y);
            }
            else {
                if (gs_currenttextrenderingmode(ctx->pgs) >= 4 && mode < 4 && ctx->text.BlockDepth != 0) {
                    /* If we are switching from a clipping mode to a non-clipping
                     * mode then behave as if we had an implicit ET to flush the
                     * accumulated text to a clip, then set the text rendering mode
                     * to the non-clip mode, and perform an implicit BT.
                     */
                    pdfi_ET(ctx);
                    gs_settextrenderingmode(ctx->pgs, mode);
                    pdfi_BT(ctx);
                }
                else
                    gs_settextrenderingmode(ctx->pgs, mode);
            }
/*        }
        else
            gs_settextrenderingmode(ctx->pgs, mode);*/
    }

    return code;
}

static int pdfi_set_Ts(pdf_context *ctx, double Ts)
{
    return gs_settextrise(ctx->pgs, Ts);
}

int pdfi_Ts(pdf_context *ctx)
{
    int code = 0;
    pdf_num *n = NULL;

    if (pdfi_count_stack(ctx) < 1) {
        pdfi_clearstack(ctx);
        return_error(gs_error_stackunderflow);
    }

    n = (pdf_num *)ctx->stack_top[-1];

    if (n->type == PDF_INT)
        code = pdfi_set_Ts(ctx, (double)n->value.i);
    else {
        if (n->type == PDF_REAL)
            code = pdfi_set_Ts(ctx, n->value.d);
        else
            code = gs_note_error(gs_error_typecheck);
    }
    pdfi_pop(ctx, 1);
    return code;
}

static int pdfi_set_Tw(pdf_context *ctx, double Tw)
{
    return gs_setwordspacing(ctx->pgs, Tw);
}

int pdfi_Tw(pdf_context *ctx)
{
    int code = 0;
    pdf_num *n = NULL;

    if (pdfi_count_stack(ctx) < 1) {
        pdfi_clearstack(ctx);
        return_error(gs_error_stackunderflow);
    }

    n = (pdf_num *)ctx->stack_top[-1];

    if (n->type == PDF_INT)
        code = pdfi_set_Tw(ctx, (double)n->value.i);
    else {
        if (n->type == PDF_REAL)
            code = pdfi_set_Tw(ctx, n->value.d);
        else
            code = gs_note_error(gs_error_typecheck);
    }
    pdfi_pop(ctx, 1);
    return code;
}

int pdfi_Tz(pdf_context *ctx)
{
    int code = 0;
    pdf_num *n = NULL;

    if (pdfi_count_stack(ctx) < 1) {
        pdfi_clearstack(ctx);
        return_error(gs_error_stackunderflow);
    }

    n = (pdf_num *)ctx->stack_top[-1];

    if (n->type == PDF_INT)
        code = gs_settexthscaling(ctx->pgs, (double)n->value.i);
    else {
        if (n->type == PDF_REAL)
            code = gs_settexthscaling(ctx->pgs, n->value.d);
        else
            code = gs_note_error(gs_error_typecheck);
    }
    pdfi_pop(ctx, 1);
    return code;
}

int pdfi_singlequote(pdf_context *ctx)
{
    int code;

    if (ctx->text.BlockDepth == 0) {
        pdfi_set_warning(ctx, 0, NULL, W_PDF_TEXTOPNOBT, "pdfi_singlequote", NULL);
    }

    if (pdfi_count_stack(ctx) < 1) {
        pdfi_clearstack(ctx);
        return_error(gs_error_stackunderflow);
    }

    code = pdfi_T_star(ctx);
    if (code < 0)
        return code;

    return pdfi_Tj(ctx);
}

int pdfi_doublequote(pdf_context *ctx)
{
    int code;
    pdf_string *s;
    pdf_num *Tw, *Tc;

    if (ctx->text.BlockDepth == 0) {
        pdfi_set_warning(ctx, 0, NULL, W_PDF_TEXTOPNOBT, "pdfi_T_doublequote", NULL);
    }

    if (pdfi_count_stack(ctx) < 3) {
        pdfi_clearstack(ctx);
        return_error(gs_error_stackunderflow);
    }

    s = (pdf_string *)ctx->stack_top[-1];
    Tc = (pdf_num *)ctx->stack_top[-2];
    Tw = (pdf_num *)ctx->stack_top[-3];
    if (s->type != PDF_STRING || (Tc->type != PDF_INT && Tc->type != PDF_REAL) ||
        (Tw->type != PDF_INT && Tw->type != PDF_REAL)) {
        pdfi_pop(ctx, 3);
        return gs_note_error(gs_error_typecheck);
    }

    if (Tc->type == PDF_INT)
        code = pdfi_set_Tc(ctx, (double)Tc->value.i);
    else
        code = pdfi_set_Tc(ctx, Tc->value.d);
    if (code < 0) {
        pdfi_pop(ctx, 3);
        return code;
    }

    if (Tw->type == PDF_INT)
        code = pdfi_set_Tw(ctx, (double)Tw->value.i);
    else
        code = pdfi_set_Tw(ctx, Tw->value.d);
    if (code < 0) {
        pdfi_pop(ctx, 3);
        return code;
    }

    code = pdfi_T_star(ctx);
    if (code < 0)
        return code;

    code = pdfi_Tj(ctx);
    pdfi_pop(ctx, 3);
    return code;
}
