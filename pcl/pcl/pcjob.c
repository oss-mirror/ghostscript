/* Copyright (C) 2001-2012 Artifex Software, Inc.
   All Rights Reserved.

   This software is provided AS-IS with no warranty, either express or
   implied.

   This software is distributed under license and may not be copied,
   modified or distributed except as expressly authorized under the terms
   of the license contained in the file LICENSE in this distribution.

   Refer to licensing information at http://www.artifex.com or contact
   Artifex Software, Inc.,  7 Mt. Lassen Drive - Suite A-134, San Rafael,
   CA  94903, U.S.A., +1(415)492-9861, for further information.
*/


/* pcjob.c -  PCL5 job control commands */
#include "std.h"
#include "gx.h"
#include "gsmemory.h"
#include "gspaint.h"
#include "gsmatrix.h"           /* for gsdevice.h */
#include "gsdevice.h"
#include "pcommand.h"
#include "pcursor.h"            /* for pcl_home_cursor() */
#include "pcstate.h"
#include "pcparam.h"
#include "pcdraw.h"
#include "pcpage.h"
#include "pjtop.h"
#include <stdlib.h>             /* for atof() */

/* Commands */

int
pcl_do_printer_reset(pcl_state_t * pcs)
{
    if (pcs->macro_level)
        return e_Range;         /* not allowed inside macro */

    /* reset the other parser in case we have gotten the
       pcl_printer_reset while in gl/2 mode. */
    pcl_implicit_gl2_finish(pcs);
    /* Print any partial page if not pclxl snippet mode. */
    if (pcs->end_page == pcl_end_page_top) {
        int code = pcl_end_page_if_marked(pcs);

        if (code < 0)
            return code;
        /* if duplex start on the front side of the paper */
        if (pcs->duplex)
            put_param1_bool(pcs, "FirstSide", true);
    }
    /* unload fonts */

    /* Reset to user default state. */
    return pcl_do_resets(pcs, pcl_reset_printer);
}

static int                      /* ESC E */
pcl_printer_reset(pcl_args_t * pargs, pcl_state_t * pcs)
{
    return pcl_do_printer_reset(pcs);
}

static int                      /* ESC % -12345 X */
pcl_exit_language(pcl_args_t * pargs, pcl_state_t * pcs)
{
    if (int_arg(pargs) != -12345)
        return e_Range;
    {
        int code = pcl_printer_reset(pargs, pcs);

        return (code < 0 ? code : e_ExitLanguage);
    }
}

static int                      /* ESC & l <num_copies> X */
pcl_number_of_copies(pcl_args_t * pargs, pcl_state_t * pcs)
{
    int i = int_arg(pargs);

    if (i < 1)
        return 0;
    pcs->num_copies = i;
    return put_param1_int(pcs, "NumCopies", i);
}

static int                      /* ESC & l <sd_enum> S */
pcl_simplex_duplex_print(pcl_args_t * pargs, pcl_state_t * pcs)
{
    int code;
    bool reopen = false;

    /* oddly the command goes to the next page irrespective of
       arguments */
    code = pcl_end_page_if_marked(pcs);
    if (code < 0)
        return code;
    pcl_home_cursor(pcs);
    switch (int_arg(pargs)) {
        case 0:
            pcs->duplex = false;
            pcs->back_side = false;
            break;
        case 1:
            pcs->duplex = true;
            pcs->back_side = false;
            pcs->bind_short_edge = false;
            break;
        case 2:
            pcs->duplex = true;
            pcs->back_side = false;
            pcs->bind_short_edge = true;
            break;
        default:
            return 0;
    }
    code = put_param1_bool(pcs, "Duplex", pcs->duplex);
    switch (code) {
        case 1:                /* reopen device */
            reopen = true;
        case 0:
            break;
        case gs_error_undefined:
            return 0;
        default:               /* error */
            if (code < 0)
                return code;
    }
    code = put_param1_bool(pcs, "FirstSide", !pcs->back_side);
    switch (code) {
        case 1:                /* reopen device */
            reopen = true;
        case 0:
            break;
        case gs_error_undefined:
            return 0;
        default:               /* error */
            if (code < 0)
                return code;
    }
    code = put_param1_bool(pcs, "BindShortEdge", pcs->bind_short_edge);
    switch (code) {
        case 1:                /* reopen device */
            reopen = true;
        case 0:
        case gs_error_undefined:
            break;
        default:               /* error */
            if (code < 0)
                return code;
    }
    return (reopen ? gs_setdevice_no_erase(pcs->pgs,
                                           gs_currentdevice(pcs->pgs)) : 0);
}

static int                      /* ESC & a <side_enum> G */
pcl_duplex_page_side_select(pcl_args_t * pargs, pcl_state_t * pcs)
{
    uint i = uint_arg(pargs);
    int code;
    /* save : because pcl_end_page() messes with it,
       or not if it was an unmarked page, so do it yourself */
    bool back_side = pcs->back_side;

    /* oddly the command goes to the next page irrespective of
       arguments */
    code = pcl_end_page_if_marked(pcs);
    if (code < 0)
        return code;
    pcl_home_cursor(pcs);

    if (i > 2)
        return 0;

    /* restore */
    pcs->back_side = back_side;
    if (pcs->duplex)
    {
        switch (i) {
            case 0:
                pcs->back_side = !pcs->back_side;
                break;
            case 1:
                pcs->back_side = false;
                break;
            case 2:
                pcs->back_side = true;
                break;
            default:
                pcs->back_side = false; /* default front */
                break;
        }
        put_param1_bool(pcs, "FirstSide", !pcs->back_side);
    }
    return 0;
}

static int                      /* ESC & l 1 T */
pcl_job_separation(pcl_args_t * pargs, pcl_state_t * pcs)
{
    int i = int_arg(pargs);

    if (i != 1)
        return 0;
        /**** NEED A DRIVER PROCEDURE FOR END-OF-JOB ****/
    return 0;
}

static int                      /* ESC & l <bin_enum> G */
pcl_output_bin_selection(pcl_args_t * pargs, pcl_state_t * pcs)
{
    uint i = uint_arg(pargs);

    if (i < 1 || i > 2)
        return e_Range;
    return put_param1_int(pcs, "OutputBin", i);
}

static int                      /* ESC & u <upi> B */
pcl_set_unit_of_measure(pcl_args_t * pargs, pcl_state_t * pcs)
{
    int num = int_arg(pargs);

    if (num <= 96)
        num = 96;
    else if (num >= 7200)
        num = 7200;
    else if (7200 % num != 0) { /* Pick the exact divisor of 7200 with the smallest */
        /* relative error. */
        static const int values[] = {
            96, 100, 120, 144, 150, 160, 180, 200, 225, 240, 288,
            300, 360, 400, 450, 480, 600, 720, 800, 900,
            1200, 1440, 1800, 2400, 3600, 7200
        };
        const int *p = values;

        while (num > p[1])
            p++;
        /* Now *p < num < p[1]. */
        if ((p[1] - (float)num) / p[1] < ((float)num - *p) / *p)
            p++;
        num = *p;
    }
    pcs->uom_cp = pcl_coord_scale / num;
    return 0;
}

/* Initialization */
static int
pcjob_do_registration(pcl_parser_state_t * pcl_parser_state,
                      gs_memory_t * mem)
{                               /* Register commands */
    DEFINE_ESCAPE_ARGS('E', "Printer Reset", pcl_printer_reset, pca_in_rtl)
        DEFINE_CLASS('%') {
        0, 'X', {
    pcl_exit_language, pca_neg_ok | pca_big_error | pca_in_rtl}},
        END_CLASS DEFINE_CLASS('&') {
    'l', 'X',
            PCL_COMMAND("Number of Copies", pcl_number_of_copies,
                            pca_neg_ignore | pca_big_clamp)}, {
    'l', 'S',
            PCL_COMMAND("Simplex/Duplex Print", pcl_simplex_duplex_print,
                            pca_neg_ignore | pca_big_ignore)}, {
    'a', 'G',
            PCL_COMMAND("Duplex Page Side Select",
                            pcl_duplex_page_side_select,
                            pca_neg_ignore | pca_big_ignore)}, {
    'l', 'T',
            PCL_COMMAND("Job Separation", pcl_job_separation,
                            pca_neg_error | pca_big_error)}, {
    'l', 'G',
            PCL_COMMAND("Output Bin Selection", pcl_output_bin_selection,
                            pca_neg_error | pca_big_error)}, {
    'u', 'D',
            PCL_COMMAND("Set Unit of Measure", pcl_set_unit_of_measure,
                            pca_neg_error | pca_big_error | pca_in_rtl)},
        END_CLASS return 0;
}

static inline float
pcl_pjl_res(pcl_state_t * pcs)
{
    pjl_envvar_t *pres = pjl_proc_get_envvar(pcs->pjls, "resolution");

    return atof(pres);
}

static int pdfa_write_list(pcl_state_t * pcs, gs_param_string_array *array_list)
{
    gs_c_param_list list;
    int code;

    gs_c_param_list_write(&list, pcs->memory);
    gs_param_list_set_persistent_keys((gs_param_list *) &list, false);
    gs_c_param_list_write_more(&list);
    code = param_write_string_array((gs_param_list *)&list, "pdfmark", array_list);
    if (code < 0)
        return code;

    gs_c_param_list_read(&list);
    code = gs_state_putdeviceparams(pcs->pgs, (gs_param_list *)&list);

    return code;
}

static int pcl_pjl_pdfa(pcl_state_t * pcs, char *profile_path)
{
    FILE *f;
    int code, bytes;
    char *profile;
    gs_param_string a[12];
    gs_param_string_array array_list;

    array_list.data = a;
    array_list.persistent = 0;

    array_list.size = 6;
    param_string_from_transient_string(a[0], "/_objdef");
    param_string_from_transient_string(a[1], "{icc_PDFA}");
    param_string_from_transient_string(a[2], "/type");
    param_string_from_transient_string(a[3], "/stream");
    param_string_from_transient_string(a[4], "[0 0 0 0 0 0]");
    param_string_from_transient_string(a[5], "OBJ");
    code = pdfa_write_list(pcs, &array_list);
    if (code < 0)
        return code;

    array_list.size = 5;
    param_string_from_transient_string(a[0], "{icc_PDFA}");
    param_string_from_transient_string(a[1], "/N");
    param_string_from_transient_string(a[2], "3");
    param_string_from_transient_string(a[3], "[0 0 0 0 0 0]");
    param_string_from_transient_string(a[4], ".PUTDICT");
    code = pdfa_write_list(pcs, &array_list);
    if (code < 0)
        return code;

    f = gp_fopen(profile_path, "rb");
    if (!f)
        return -1;

    gp_fseek_64(f, 0, SEEK_END);
    bytes = gp_ftell_64(f);
    profile = (char *)gs_alloc_bytes(pcs->memory, bytes, "PJL, ICC profile");
    if (!profile)
        return -1;

    gp_fseek_64(f, 0, SEEK_SET);
    fread(profile, 1, bytes, f);
    fclose(f);

    array_list.size = 4;
    param_string_from_transient_string(a[0], "{icc_PDFA}");
    a[1].data = (const byte *)profile;
    a[1].size = bytes;
    a[1].persistent = 0;
    param_string_from_transient_string(a[2], "[0 0 0 0 0 0]");
    param_string_from_transient_string(a[3], ".PUTSTREAM");
    code = pdfa_write_list(pcs, &array_list);
    gs_free_object(pcs->memory, profile, "PJL, ICC profile");
    if (code < 0)
        return code;

    array_list.size = 6;
    param_string_from_transient_string(a[0], "/_objdef");
    param_string_from_transient_string(a[1], "{OutputIntent_PDFA}");
    param_string_from_transient_string(a[2], "/type");
    param_string_from_transient_string(a[3], "/dict");
    param_string_from_transient_string(a[4], "[0 0 0 0 0 0]");
    param_string_from_transient_string(a[5], "OBJ");
    code = pdfa_write_list(pcs, &array_list);
    if (code < 0)
        return code;

    array_list.size = 11;
    param_string_from_transient_string(a[0], "{OutputIntent_PDFA}");
    param_string_from_transient_string(a[1], "/S");
    param_string_from_transient_string(a[2], "/GTS_PDFA1");
    param_string_from_transient_string(a[3], "/Type");
    param_string_from_transient_string(a[4], "/OutputIntent");
    param_string_from_transient_string(a[5], "/DestOutputProfile");
    param_string_from_transient_string(a[6], "{icc_PDFA}");
    param_string_from_transient_string(a[7], "/OutputConditionIdentifier");
    param_string_from_transient_string(a[8], "(sRGB)");
    param_string_from_transient_string(a[9], "[0 0 0 0 0 0]");
    param_string_from_transient_string(a[10], ".PUTDICT");
    code = pdfa_write_list(pcs, &array_list);
    if (code < 0)
        return code;

    array_list.size = 5;
    param_string_from_transient_string(a[0], "{Catalog}");
    param_string_from_transient_string(a[1], "/OutputIntents");
    param_string_from_transient_string(a[2], "[ {OutputIntent_PDFA} ]");
    param_string_from_transient_string(a[3], "[0 0 0 0 0 0]");
    param_string_from_transient_string(a[4], ".PUTDICT");
    code = pdfa_write_list(pcs, &array_list);

    return code;
}

static void
pcjob_do_reset(pcl_state_t * pcs, pcl_reset_type_t type)
{
    if (type & (pcl_reset_initial | pcl_reset_printer)) {
        /* check for a resolution setting from PJL and set it right
           away, there is no pcl command to set the resolution so a
           state variable is not needed.  Don't override -r from the command line. */
        if (!pcs->res_set_on_command_line) {
            gx_device *pdev = gs_currentdevice(pcs->pgs);
            float res[2];
            gs_point device_res;

            device_res.x = pdev->HWResolution[0];
            device_res.y = pdev->HWResolution[1];

            /* resolution is always a single number in PJL */
            res[0] = res[1] = pcl_pjl_res(pcs);

            /* 
             * again we only have a single number from PJL so just
             *  check the width
             */

            if (res[0] != 0 && res[0] != device_res.x) {
                put_param1_float_array(pcs, "HWResolution", res);
                gs_erasepage(pcs->pgs);
            }
        }

        pcs->num_copies = pjl_proc_vartoi(pcs->pjls,
                                          pjl_proc_get_envvar(pcs->pjls,
                                                              "copies"));
        pcs->duplex =
            !pjl_proc_compare(pcs->pjls,
                              pjl_proc_get_envvar(pcs->pjls, "duplex"),
                              "off") ? false : true;
        pcs->bind_short_edge =
            !pjl_proc_compare(pcs->pjls,
                              pjl_proc_get_envvar(pcs->pjls, "binding"),
                              "longedge") ? false : true;
        pcs->back_side = false;
        pcs->output_bin = 1;
        put_param1_bool(pcs, "Duplex", pcs->duplex);
        put_param1_bool(pcs, "FirstSide", !pcs->back_side);
        put_param1_bool(pcs, "BindShortEdge", pcs->bind_short_edge);

        {
            pjl_envvar_t *pres;

            pres = pjl_proc_get_envvar(pcs->pjls, "pdfa_profile");

            if (strlen(pres) > 0) {
                pcl_pjl_pdfa(pcs, pres);
            }
        }
    }

    if (type & (pcl_reset_initial | pcl_reset_printer | pcl_reset_overlay)) {
        /* rtl always uses native units for user units.  The hp
           documentation does not say what to do if the resolution is
           assymetric... */
        pcl_args_t args;

        if (pcs->personality == rtl) {
            float res[2];

            /* resolution is always a single number in PJL */
            res[0] = res[1] = pcl_pjl_res(pcs);

            if (res[0] != 0)
                arg_set_uint(&args, (uint)res[0]);
            else
                arg_set_uint(&args,
                             (uint) gs_currentdevice(pcs->pgs)->
                             HWResolution[0]);
        } else
            arg_set_uint(&args, 300);
        pcl_set_unit_of_measure(&args, pcs);
    }
}
const pcl_init_t pcjob_init = {
    pcjob_do_registration, pcjob_do_reset, 0
};
