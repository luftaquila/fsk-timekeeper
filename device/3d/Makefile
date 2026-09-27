# Printable housings. Requires OpenSCAD.
#
#   make          render master-{base,lid}.stl and sensor-{base,lid}.stl
#   make clean    remove the STLs

OPENSCAD ?= openscad
PARTS := master-base master-lid sensor-base sensor-lid

.PHONY: all clean
all: $(addsuffix .stl,$(PARTS))

%-base.stl: %.scad
	$(OPENSCAD) -o $@ -D 'part="base"' $<

%-lid.stl: %.scad
	$(OPENSCAD) -o $@ -D 'part="lid"' $<

clean:
	rm -f *.stl
