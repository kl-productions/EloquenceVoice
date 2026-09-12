# Read after openevv's own Makefile: `gmake -f Makefile -f ios.mk ios-lib`.
# The engine archive plus lib/eci_api.c, which holds the published eci* names
# and the constructor that starts the engine, as one static library for an app.
# The object goes in $(BUILD) rather than $(OBJDIR), because the archive rule
# deletes any object there it did not make.
.PHONY: ios-lib
IOSLIB := $(BUILD)/libopenevv-ios.a

ios-lib: $(IOSLIB)

$(IOSLIB): lib/eci_api.c $(BUILD)/libevv$(SUF).a
	@$(CC) $(ALL_CFLAGS) -c lib/eci_api.c -o $(BUILD)/eci_api_ios.o
	@rm -f $@
	@cp $(BUILD)/libevv$(SUF).a $@
	@ar rs $@ $(BUILD)/eci_api_ios.o
	@echo "built $@"
