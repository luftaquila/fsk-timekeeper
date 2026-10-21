// Sensor tray: open-top box, closed at one end, open and cut down on a diagonal at the other.
// Floor 38 (X) x 60 (Y); the 60 x 50 side walls are 50 mm tall up to Y=45, then slope down to
// 35 mm at Y=60:
//   Y=0 end  : closed wall (막힌 벽), full 50 mm
//   Y=60 end : open (열린 벽), no end wall
//   top      : open
//   bottom   : floor plate

$fn = 64;
eps = 0.1;

// ---- Outer envelope ----
width   = 38;   // X — bottom short dimension
length  = 60;   // Y — bottom long dimension / side-wall length
height  = 50;   // Z — side-wall height (full, at the closed end)

// ---- Corner slice on the open end ----
cut_y   = 45;   // top stays at full height up to this length, then slopes down
cut_h   = 35;   // height at the open end (Y = length)

// ---- Shell ----
wall    = 5;    // side + closed-end wall thickness
floor_t = 8;    // floor plate thickness
round_r = 2;    // fillet radius for all convex edges (outer + inner rim/mouth)
round_pad = round_r + 2;   // bounding-box margin for the opening operation

// ---- Tripod mount: plain floor through-hole, dead-centre, no boss ----
tri_pilot_d = 5.7;  // 1/4"-20 self-tap pilot, as in traffic/device/wireless/3d/master.scad

// ---- M3 self-tap holes (floor, near the +Y open end) ----
m3_pilot   = 2.6;   // repo-standard M3 self-tap pilot Ø (PLA/PETG)
m3_inset_y = 6.5;   // in from the +Y edge toward -Y
m3_gap     = 10;    // centre-to-centre spacing along X
m3_y       = length - m3_inset_y;
m3_xs      = [width / 2 - m3_gap / 2, width / 2 + m3_gap / 2];
// One more M3 self-tap hole on the width centreline, 41.5 mm further -Y.
m3_single_y = m3_y - 41.5;

// ---- Side-wall holes (horizontal, into the cavity): Z from the bottom, Y in from the +Y edge ----
holeA_d = 8;                // -X wall (X = 0)
holeA_z = 22;
holeA_y = length - 27.5;    // 32.5
holeB_d = 5;                // +X wall (X = width)
holeB_z = 12.5;
holeB_y = length - 27;      // 33

// Outer side profile in (Y, Z), extruded across the X width.
outer_prof = [
    [0,      0     ],
    [length, 0     ],
    [length, cut_h ],
    [cut_y,  height],
    [0,      height],
];

// Inner cavity in (Y, Z): above the floor, from Y = wall out past the open end and the top.
inner_prof = [
    [wall,       floor_t   ],
    [length+eps, floor_t   ],
    [length+eps, height+eps],
    [wall,       height+eps],
];

// Extrude a (Y, Z) profile along X for the given width.
module extrude_x(w, prof) {
    rotate([90, 0, 90])
        linear_extrude(w)
            polygon(prof);
}

// Sharp shell: outer solid minus the cavity; shell_rounded() rounds its edges.
module shell_sharp() {
    difference() {
        extrude_x(width, outer_prof);
        // Cavity inset by `wall` on both long sides (X), leaving the two side walls.
        translate([wall, 0, 0])
            extrude_x(width - 2 * wall, inner_prof);
    }
}

// Bounding box with round_pad clearance on every side, for the opening.
module bbox() {
    translate([-round_pad, -round_pad, -round_pad])
        cube([width + 2 * round_pad, length + 2 * round_pad, height + 2 * round_pad]);
}

// Morphological opening (erode, then dilate by round_r): rounds every convex edge, keeps concave
// corners, flat faces and the envelope. Erodes via the box complement (no native 3D erode).
module shell_rounded() {
    minkowski() {
        difference() {
            bbox();
            minkowski() {
                difference() { bbox(); shell_sharp(); }
                sphere(r = round_r, $fn = 24);
            }
        }
        sphere(r = round_r, $fn = 24);
    }
}

module sensor_tray() {
    difference() {
        shell_rounded();
        // Self-tapping tripod pilot through the floor, dead-centre.
        translate([width / 2, length / 2, -eps])
            cylinder(d = tri_pilot_d, h = floor_t + 2 * eps);
        // M3 self-tap holes: a pair near the +Y edge + one on the centreline.
        for (x = m3_xs)
            translate([x, m3_y, -eps])
                cylinder(d = m3_pilot, h = floor_t + 2 * eps);
        translate([width / 2, m3_single_y, -eps])
            cylinder(d = m3_pilot, h = floor_t + 2 * eps);
        // Side-wall holes, horizontal (along X) through each long wall.
        translate([-eps, holeA_y, holeA_z])
            rotate([0, 90, 0]) cylinder(d = holeA_d, h = wall + 2 * eps);
        translate([width - wall - eps, holeB_y, holeB_z])
            rotate([0, 90, 0]) cylinder(d = holeB_d, h = wall + 2 * eps);
    }
}

sensor_tray();
