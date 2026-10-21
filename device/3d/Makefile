# Printable housings and sensor holder (requires OpenSCAD).
#   make          render master.stl, sensor.stl (base and lid together), and sensor-holder.stl
#   make clean    remove the STLs

OPENSCAD ?= openscad
PARTS := master sensor sensor-holder

.PHONY: all clean
all: $(addsuffix .stl,$(PARTS))

sensor-holder.stl: sensor-holder.scad
	$(OPENSCAD) -o $@ $<

%.stl: %.scad
	$(OPENSCAD) -o $@ -D 'part="both"' $<

%-base.stl: %.scad
	$(OPENSCAD) -o $@ -D 'part="base"' $<

%-lid.stl: %.scad
	$(OPENSCAD) -o $@ -D 'part="lid"' $<

clean:
	rm -f *.stl
