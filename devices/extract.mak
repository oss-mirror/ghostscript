extract_cc = $(CC) $(CCFLAGS) -DEXTRACT_CV=0 $(I_)$(EXTRACT_DIR)/include$(_I) $(I_)$(ZSRCDIR)$(_I) $(O_)
extract_out_prefix = $(GLOBJDIR)$(D)extract_

$(extract_out_prefix)alloc.$(OBJ):          $(EXTRACT_DIR)/src/alloc.c
	$(extract_cc)$@ $(C_) $(EXTRACT_DIR)/src/alloc.c

$(extract_out_prefix)astring.$(OBJ):        $(EXTRACT_DIR)/src/astring.c
	$(extract_cc)$@ $(C_) $(EXTRACT_DIR)/src/astring.c

$(extract_out_prefix)buffer.$(OBJ):         $(EXTRACT_DIR)/src/buffer.c
	$(extract_cc)$@ $(C_) $(EXTRACT_DIR)/src/buffer.c

$(extract_out_prefix)document.$(OBJ):       $(EXTRACT_DIR)/src/document.c
	$(extract_cc)$@ $(C_) $(EXTRACT_DIR)/src/document.c

$(extract_out_prefix)docx.$(OBJ):           $(EXTRACT_DIR)/src/docx.c
	$(extract_cc)$@ $(C_) $(EXTRACT_DIR)/src/docx.c

$(extract_out_prefix)docx_template.$(OBJ):  $(EXTRACT_DIR)/src/docx_template.c
	$(extract_cc)$@ $(C_) $(EXTRACT_DIR)/src/docx_template.c

$(extract_out_prefix)extract.$(OBJ):        $(EXTRACT_DIR)/src/extract.c
	$(extract_cc)$@ $(C_) $(EXTRACT_DIR)/src/extract.c

$(extract_out_prefix)join.$(OBJ):           $(EXTRACT_DIR)/src/join.c
	$(extract_cc)$@ $(C_) $(EXTRACT_DIR)/src/join.c

$(extract_out_prefix)html.$(OBJ):            $(EXTRACT_DIR)/src/html.c
	$(extract_cc)$@ $(C_) $(EXTRACT_DIR)/src/html.c

$(extract_out_prefix)mem.$(OBJ):            $(EXTRACT_DIR)/src/mem.c
	$(extract_cc)$@ $(C_) $(EXTRACT_DIR)/src/mem.c

$(extract_out_prefix)odt.$(OBJ):            $(EXTRACT_DIR)/src/odt.c
	$(extract_cc)$@ $(C_) $(EXTRACT_DIR)/src/odt.c

$(extract_out_prefix)odt_template.$(OBJ):   $(EXTRACT_DIR)/src/odt_template.c
	$(extract_cc)$@ $(C_) $(EXTRACT_DIR)/src/odt_template.c

$(extract_out_prefix)outf.$(OBJ):           $(EXTRACT_DIR)/src/outf.c
	$(extract_cc)$@ $(C_) $(EXTRACT_DIR)/src/outf.c

$(extract_out_prefix)sys.$(OBJ):           $(EXTRACT_DIR)/src/sys.c
	$(extract_cc)$@ $(C_) $(EXTRACT_DIR)/src/sys.c

$(extract_out_prefix)text.$(OBJ):           $(EXTRACT_DIR)/src/text.c
	$(extract_cc)$@ $(C_) $(EXTRACT_DIR)/src/text.c

$(extract_out_prefix)xml.$(OBJ):            $(EXTRACT_DIR)/src/xml.c
	$(extract_cc)$@ $(C_) $(EXTRACT_DIR)/src/xml.c

$(extract_out_prefix)zip.$(OBJ):            $(EXTRACT_DIR)/src/zip.c
	$(extract_cc)$@ $(C_) $(EXTRACT_DIR)/src/zip.c

EXTRACT_OBJS = \
	$(extract_out_prefix)alloc.$(OBJ) \
	$(extract_out_prefix)astring.$(OBJ) \
	$(extract_out_prefix)buffer.$(OBJ) \
	$(extract_out_prefix)document.$(OBJ) \
	$(extract_out_prefix)docx.$(OBJ) \
	$(extract_out_prefix)docx_template.$(OBJ) \
	$(extract_out_prefix)extract.$(OBJ) \
	$(extract_out_prefix)html.$(OBJ) \
	$(extract_out_prefix)join.$(OBJ) \
	$(extract_out_prefix)mem.$(OBJ) \
	$(extract_out_prefix)odt.$(OBJ) \
	$(extract_out_prefix)odt_template.$(OBJ) \
	$(extract_out_prefix)outf.$(OBJ) \
	$(extract_out_prefix)sys.$(OBJ) \
	$(extract_out_prefix)text.$(OBJ) \
	$(extract_out_prefix)xml.$(OBJ) \
	$(extract_out_prefix)zip.$(OBJ) \
