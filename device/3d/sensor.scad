// PCB Housing — FSK Timekeeper (wireless LoRa timing node) — SENSOR variant (sensor I/O only, no light)
// Board: 92 x 57 mm, corner r 3, 4x M2 mounting holes (86 x 51 rectangle),
// all tall components on the TOP side (18650 holder dominates the cavity height).
//
// Two parts:
//   lid  (본체 하우징)  — fully-rounded outer shell (ALL edges filleted) + ceiling + 4 self-tapping
//                         posts + connector cutouts + tripod boss. Open at the bottom.
//   base (바닥판)        — plate that inserts UP into the bottom opening of the lid (tucked under),
//                         carries the PCB on standoffs, fixed by the 4 screws into the lid posts.
//
// Assembly: M2 self-tapping screws inserted from OUTSIDE the base bottom pass through the
// base plate + PCB and self-tap into the posts hanging from the lid ceiling, pulling the
// base + PCB up against the posts and seating the base flush in the lid's bottom opening.
//
// Wall features (SENSOR):
//   - left (-X): centred SMA antenna
//   - right (+X): tripod mount at the original position
//   - front (-Y): USB-C, J1 sensor and power toggle
//   - back (+Y): blank
// Coordinate frame: PCB centred at XY origin; Z=0 at assembly bottom.
// Current KiCad centre: (147.1648, 98.03); model = (boardX-cx, cy-boardY).

/* [Rendering] */
part = "exploded";   // [base, lid, both, assembled, exploded]
show_pcb = true;     // PCB + 18650 reference in 'assembled' / 'exploded'
show_switch = true;  // MTS-102 reference attached to lid; F5 preview only
explode = 30;        // separation gap between stacked parts in 'exploded'

/* [PCB] */
pcb_l = 92;        // board length (X)
pcb_w = 57;        // board width  (Y)
pcb_r = 3;         // board corner radius
pcb_th = 1.6;      // board thickness
mount_inset = 3;   // mounting-hole inset from each edge (= corner radius)
pcb_cx = 147.1648;
pcb_cy = 98.03;

/* [Shell] */
wall = 2.5;        // side wall thickness
lid_th = 2;        // lid ceiling thickness
base_th = 3;       // base plate thickness (>= edge_r so its bottom edge can be filleted)
pcb_clear = 0.5;   // gap between PCB edge and wall inner face
standoff_h = 2.5;  // PCB bottom height above base top
edge_r = 2.5;      // fillet radius for ALL outer edges of the body
batt_h = 21;       // PCB TOP to installed CELL TOP; retained at user-specified 21 mm
ra01_h = 6;        // assumed module height above PCB
gps_h = 6.5;       // installed GPS height above PCB TOP

/* [Battery retention] */
batt_d = 18.5;             // installed cell diameter, including wrapper
batt_x = 147.1 - pcb_cx;
batt_y = pcb_cy - 80.45;
batt_block_l = 24;         // single central contact, away from both terminals
batt_block_w = 12;
batt_block_wrap = 2;       // saddle extends this far below cell crown
batt_top_trim = 1;         // trim print-top shoulders by 1 mm in Z; horizontal faces
batt_contact_gap = 0;      // nominal touch; positive adds clearance, never force the cell

/* [PCB-mount screws: base -> lid post, M2 self-tapping] */
post_d = 5.5;          // lid post outer diameter
post_style = "corner"; // [corner, rib, plain]
post_pad = 7;          // "corner" style: corner pad size
post_fillet = 2;       // "corner" style: rounding of the pad's cavity edge
post_rib = 2.4;        // "rib" style: rib thickness
post_pilot_d = 1.7;    // M2 self-tap pilot bore in post
post_pilot_depth = 16; // pilot depth up into post
so_d = 5.5;            // base standoff outer diameter
screw_clear_d = 2.6;   // M2 shaft clearance (through base + PCB)
head_d = 4.4;          // counterbore for screw head (base bottom)
head_h = 2;            // counterbore depth
base_fit = 0.3;        // base clearance per side inside the lid bottom opening

/* [J1 cutout — front/-Y wall] */
// J1 at (132.724,115.63), rotated 180 degrees. Body local X=-2.7..6.9.
// Use body centre +2.1, not the footprint origin. No J2 on the current PCB.
molex_x = 132.724 - 2.1 - pcb_cx;
molex_front_y = pcb_cy - (115.63 + 13.9);
molex_cut_w = 11;
molex_cut_h = 11;
molex_latch_w = 4;
molex_latch_h = 4.5;

/* [USB-C cutout — front/-Y wall] */
// U1 at (154.74401,124.704948), rotated 180 degrees.
// USB outline local X=3.12001..12.11999 -> centre 7.62.
usb_x = 154.74401 - 7.62 - pcb_cx;
usb_front_y = pcb_cy - (124.704948 + 1.95244);
usb_cut_w = 13;
usb_cut_h = 7;
usb_above_bot = 6.4;       // measured port centre above PCB BOTTOM

/* [Power toggle — front/-Y wall, on J1's side away from USB] */
sw_d = 6;
sw_post_gap = 0.5;         // body clearance to the adjacent corner mounting post
sw_z = 19;                // centre of mounting hole
// MTS-102 manufacturer drawing: https://www.piekarz.pl/pdf/51315/
// Mounted long side along X, bushing axis along Y, short side along Z.
sw_body_x = 13.2;
sw_body_y = 9.5;
sw_body_z = 7.9;
sw_terminal_l = 4;
sw_body_setback = 0;       // body flush against inner wall; increase for an inner nut
// sw_x places the body next to the corner post with sw_post_gap clearance.

/* [SMA antenna — centre of left wall] */
ant_d = 6.5;

/* [Tripod mount — right/+X wall] */
// Keep the original position. GPS is 6.5 mm above PCB; the box starts
// 8 mm above PCB, leaving 1.5 mm vertical clearance over the GPS module.
tri_box_w = 12;
tri_y = -1.5;
tri_box_d = 10;
tri_pilot_d = 5.7;
tri_depth = 8;
tri_gap = 2;

/* [Quality] */
$fn = 64;

// ---------- derived ----------
inner_l = pcb_l + 2*pcb_clear;
inner_w = pcb_w + 2*pcb_clear;
outer_l = inner_l + 2*wall;
outer_w = inner_w + 2*wall;
inner_r = pcb_r + pcb_clear;
outer_r = inner_r + wall;

base_l = inner_l - 2*base_fit;       // plugs the bottom opening
base_w = inner_w - 2*base_fit;
base_r = inner_r - base_fit;

pcb_bot_z = base_th + standoff_h;    // PCB underside
pcb_top_z = pcb_bot_z + pcb_th;      // PCB top surface
usb_z     = pcb_bot_z + usb_above_bot;   // USB-C port centre Z (measured from the PCB bottom face)
// height: max of battery, latch-slot reach, and "tripod bore at face centre while its box clears the Ra-01"
cavity    = max(batt_h + tri_gap, molex_cut_h + molex_latch_h + 2, 2*(pcb_top_z + ra01_h + 4 + tri_pilot_d/2) - pcb_top_z - lid_th);
ceil_z    = pcb_top_z + cavity;      // lid inner ceiling
outer_h   = ceil_z + lid_th;         // overall height
post_len  = ceil_z - pcb_top_z;      // = cavity

tri_hole_z  = outer_h/2;                          // 1/4" bore at the vertical CENTRE of the right face
tri_box_bot = pcb_top_z + ra01_h + 2;             // original height: 8 mm above PCB, clears GPS by 1.5 mm
ant_y       = 0;
ant_z       = outer_h/2;
// Exclude the corner post from the usable space on J1's side away from USB.
sw_space_left = max(-inner_l/2 + (post_style == "corner" ? post_pad : 0),
                    -pcb_l/2 + mount_inset + post_d/2);
sw_space_right = molex_x - molex_cut_w/2;
sw_x = sw_space_left + sw_post_gap + sw_body_x/2;
assert(sw_post_gap > 0, "Keep clearance between the switch and corner post");
assert(sw_x+sw_body_x/2 < sw_space_right, "Toggle body overlaps the J1 aperture");

batt_top_z = pcb_top_z + batt_h;
batt_axis_z = batt_top_z - batt_d/2;
batt_block_bot = batt_top_z + batt_contact_gap - batt_block_wrap;
assert(batt_contact_gap >= 0, "Do not preload the battery with rigid interference");
assert(batt_block_bot < ceil_z, "Battery retainer must reach the lid");

mx = pcb_l/2 - mount_inset;          // 43
my = pcb_w/2 - mount_inset;          // 25.5
mount_pos = [[-mx,-my],[mx,-my],[-mx,my],[mx,my]];

eps = 0.01;

// ---------- helpers ----------
module rrect(l,w,r) offset(r=r) offset(r=-r) square([l,w], center=true);
module rbox(l,w,h,r) linear_extrude(h) rrect(l,w,r);
// cylinder along +X from x0, centred on (y,z)
module xcyl(x0,y,z,len,d) translate([x0,y,z]) rotate([0,90,0]) cylinder(h=len, d=d);

// box with ALL outer edges rounded (vertical corners = vr, every edge fillet = er); needs h > 2*er
module rbox_all(l,w,h,vr,er) {
    minkowski() {
        translate([0,0,er])
            linear_extrude(h - 2*er) rrect(l - 2*er, w - 2*er, max(0.1, vr - er));
        sphere(r=er, $fn=28);
    }
}

// self-tapping posts (ceiling -> PCB top) at original post_d, integrated into the corners.
// post_style: "corner" = post embedded in a solid filled corner pad (rounded cavity edge);
//             "rib"    = post cylinder + two slim ribs to the adjacent walls;
//             "plain"  = bare cylinder.
module posts() {
    if (post_style == "rib") {
        for (p=mount_pos) {
            sx = sign(p[0]); sy = sign(p[1]);
            translate([p[0],p[1],pcb_top_z]) cylinder(h=post_len, d=post_d);
            translate([0,0,pcb_top_z]) linear_extrude(post_len) {
                hull() { translate(p) circle(d=post_rib); translate([sx*inner_l/2, p[1]]) circle(d=post_rib); }
                hull() { translate(p) circle(d=post_rib); translate([p[0], sy*inner_w/2]) circle(d=post_rib); }
            }
        }
    } else if (post_style == "corner") {
        translate([0,0,pcb_top_z]) linear_extrude(post_len)
            for (p=mount_pos) {
                sx = sign(p[0]); sy = sign(p[1]);
                union() {
                    translate(p) circle(d=post_d);
                    offset(r=post_fillet) offset(r=-post_fillet) intersection() {
                        translate([sx*inner_l/2, sy*inner_w/2]) square(2*post_pad, center=true);
                        rrect(inner_l, inner_w, inner_r);
                    }
                }
            }
    } else {
        for (p=mount_pos) translate([p[0],p[1],pcb_top_z]) cylinder(h=post_len, d=post_d);
    }
}

// Ceiling-integrated box; prints from its roof down without supports.
module tripod_box() {
    translate([inner_l/2 - tri_box_d, tri_y - tri_box_w/2, tri_box_bot])
        cube([tri_box_d, tri_box_w, ceil_z - tri_box_bot + eps]);
}

// One broad central saddle stops upward cell movement, clear of both terminals.
// Trim both print-top shoulders at one constant Z plane. No bevel or fillet.
// The centre retains the original battery-contact arc and contact height.
module battery_block() {
    flat_z = batt_block_bot + batt_top_trim;
    assert(batt_top_trim > 0 && batt_top_trim < batt_block_wrap,
           "Leave a shallow battery arc between the horizontal shoulders");
    difference() {
        translate([batt_x-batt_block_l/2,batt_y-batt_block_w/2,flat_z])
            cube([batt_block_l,batt_block_w,ceil_z-flat_z+eps]);
        translate([batt_x,batt_y,batt_axis_z+batt_contact_gap])
            rotate([0,90,0]) cylinder(h=batt_block_l+2*eps,d=batt_d,center=true,$fn=128);
    }
}

// ============================================================
//  LID  (본체 하우징) — fully-rounded shell, open bottom
// ============================================================
module lid() {
    difference() {
        union() {
            difference() {
                rbox_all(outer_l, outer_w, outer_h, outer_r, edge_r);          // fully-rounded body
                translate([0,0,-eps]) rbox(inner_l, inner_w, ceil_z+eps, inner_r); // cavity (open bottom)
            }
            // self-tapping posts (fused to corners) + ceiling-integrated tripod box
            posts();
            tripod_box();
            battery_block();
        }

        // post pilot bores
        for (p=mount_pos) translate([p[0],p[1],pcb_top_z-eps]) cylinder(h=post_pilot_depth, d=post_pilot_d);

        // J1 front-wall aperture and latch slot; rotate the old side opening by 90 degrees.
        translate([molex_x-molex_cut_w/2, -outer_w/2-eps, pcb_top_z-0.5])
            cube([molex_cut_w, wall+pcb_clear+1+eps, molex_cut_h+0.5]);
        translate([molex_x-molex_latch_w/2, -outer_w/2-eps, pcb_top_z-0.5+molex_cut_h])
            cube([molex_latch_w, wall+pcb_clear+1+eps, molex_latch_h]);
        // USB-C (-Y wall) — opening with r=1 rounded corners, centred at usb_z
        translate([usb_x, -inner_w/2+eps, usb_z])
            rotate([90,0,0])
                linear_extrude(wall+pcb_clear+1+eps) rrect(usb_cut_w, usb_cut_h, 1);
        // toggle switch bushing (-Y wall, left, clear of the Molex)
        translate([sw_x, -inner_w/2+eps, sw_z]) rotate([90,0,0])
            cylinder(h=wall+pcb_clear+1+eps, d=sw_d);
        // SMA antenna bulkhead at the horizontal and vertical centre of the left wall
        xcyl(-outer_l/2-eps, ant_y, ant_z, wall+pcb_clear+1+2*eps, ant_d);
        // tripod self-tap pilot (+X / right wall, blind, horizontal into the box, centred Y)
        xcyl(outer_l/2 - tri_depth - eps, tri_y, tri_hole_z, tri_depth + 2*eps, tri_pilot_d);
    }
}

// ============================================================
//  BASE  (바닥판) — plugs the bottom opening, rounded bottom edge
// ============================================================
module base() {
    difference() {
        union() {
            // flat plate that sits flush in the lid's bottom opening (rounded rim is on the lid)
            rbox(base_l, base_w, base_th, base_r);
            for (p=mount_pos)
                translate([p[0],p[1],base_th-eps]) cylinder(h=standoff_h+eps, d=so_d);
        }
        // screw clearance + head counterbore (entered from base bottom)
        for (p=mount_pos) translate([p[0],p[1],0]) {
            translate([0,0,-eps]) cylinder(h=pcb_bot_z+2*eps, d=screw_clear_d);
            translate([0,0,-eps]) cylinder(h=head_h+eps,     d=head_d);
        }
    }
}

// ============================================================
//  REFERENCE (assembled / exploded views)
// ============================================================
// PCB + 18650 cell reference — drawn ONLY in F5 preview ($preview); never in F6 render / STL export
module pcb_ref() {
    if ($preview) {
        color([0,0.45,0.15,0.55]) translate([0,0,pcb_bot_z]) rbox(pcb_l,pcb_w,pcb_th,pcb_r);
        // Holder courtyard is an envelope, not solid plastic; cell is shown separately.
        color([0.75,0.75,0.78,0.7])
            translate([batt_x,batt_y,batt_axis_z]) rotate([0,90,0]) cylinder(h=65,d=batt_d,center=true);
        %translate([batt_x,batt_y,pcb_top_z+batt_h/2]) cube([78.06,21.66,batt_h],center=true);
        color([0.75,0.75,0.78,0.45]) for (dx=[-41.5,41.5])
            translate([batt_x+dx,batt_y,pcb_top_z+batt_h/2]) cube([4.9,7.5,batt_h],center=true);
        color([0.2,0.2,0.28,0.5])
            translate([167.219376-pcb_cx,pcb_cy-107.884,pcb_top_z+ra01_h/2])
                cube([18.5,18,ra01_h],center=true);
        color([0.85,0.5,0.15,0.5])
            translate([molex_x-4.8,molex_front_y,pcb_top_z]) cube([9.6,12.8,11]);
        color([0.55,0.55,0.6,0.6])
            translate([usb_x,usb_front_y+3.25,usb_z]) cube([9,6.5,3.26],center=true);
    }
}

// The switch belongs to the lid, so it follows the lid in exploded/print views.
// Preview only: never fuse this reference body or its terminals into exported STL.
module switch_ref() {
    if ($preview) {
        body_y = -inner_w/2 + sw_body_setback;
        color([0.1,0.3,0.95])
            translate([sw_x-sw_body_x/2,body_y,sw_z-sw_body_z/2])
                cube([sw_body_x,sw_body_y,sw_body_z]);
        color([0.75,0.77,0.8]) for (dx=[-4.7,0,4.7])
            translate([sw_x+dx-0.4,body_y+sw_body_y,sw_z-1])
                cube([0.8,sw_terminal_l,2]);
    }
}

// Apply shell colour only to the shell: an outer color() overrides reference colours.
module lid_view() {
    color("khaki", 0.85) lid();
    if (show_switch) switch_ref();
}

// ============================================================
//  RENDER
// ============================================================
if (part=="base") base();
else if (part=="lid")
    translate([0,0,outer_h]) rotate([180,0,0]) lid_view();          // flipped onto its ceiling for printing
else if (part=="both") {
    translate([-(outer_l/2+8),0,0]) base();
    translate([ (outer_l/2+8),0,outer_h]) rotate([180,0,0]) lid_view();
}
else if (part=="assembled") {
    color("lightblue") base();
    lid_view();
    if (show_pcb) pcb_ref();
}
else if (part=="exploded") {
    // stacked bottom -> top along Z: base, PCB, lid
    color("lightblue") base();
    if (show_pcb) translate([0,0,explode]) pcb_ref();
    translate([0,0,2*explode]) lid_view();
}
