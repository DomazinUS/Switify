TITLE_ID := 4200000000535754
VERSION := 0.5.0

.PHONY: all module overlay probe dist clean

all: module overlay probe

module:
	$(MAKE) -C sysmodule

overlay:
	$(MAKE) -C overlay

probe:
	$(MAKE) -C probe

dist: module overlay probe
	rm -rf dist
	mkdir -p dist/atmosphere/contents/$(TITLE_ID)
	mkdir -p dist/switch/.overlays
	cp sysmodule/switify-sysmodule.nsp \
	   dist/atmosphere/contents/$(TITLE_ID)/exefs.nsp
	cp sysmodule/toolbox.json \
	   dist/atmosphere/contents/$(TITLE_ID)/toolbox.json
	cp overlay/switify-overlay.ovl \
	   dist/switch/.overlays/Switify.ovl

clean:
	$(MAKE) -C sysmodule clean
	$(MAKE) -C overlay clean
	$(MAKE) -C probe clean
	rm -rf build_host dist
