# Evaluates the vendor's Kbuild (qcacld-3.0) for this port's profile and writes the object list and compiler flags.
# make -f objlist.mk -C <qcacld-3.0> list OUT=<dir> <overrides from config/disabled.txt>
include Kbuild
list:
	@echo $(OBJS) | tr ' ' '\n' | grep -v '^$$' > $(OUT)/objs.txt
	@echo $(ccflags-y) $(EXTRA_CFLAGS) | tr ' ' '\n' | grep -v '^$$' > $(OUT)/cflags.txt
