// PCB housing for the FSK Timekeeper, MASTER variant (USB only, no light/sensor I/O).
// Board: 92 x 57 mm, corner r 3, 4x M2 holes on an 86 x 51 rectangle; all tall parts on TOP
// (the 18650 holder sets the cavity height).
//
// Parts:
//   lid  (본체 하우징) — fully-rounded shell + ceiling + 4 self-tapping posts + cutouts
//                        + tripod boss; open at the bottom
//   base (바닥판)      — plate inserted up into the lid's bottom opening; carries the PCB
//                        on standoffs
// Assembly: M2 self-tapping screws from outside the base bottom go through base + PCB into the
// lid posts, pulling both up and seating the base flush in the opening.
//
// Walls (MASTER):
//   left  (-X): centred SMA antenna; GPS antenna slide bracket on the inner ceiling
//   right (+X): tripod mount at its original position
//   front (-Y): USB-C only (J1 and toggle not fitted)
//   back  (+Y): blank
// Frame: PCB centred at the XY origin, Z=0 at the assembly bottom.
// KiCad board centre (147.1648, 98.03); model = (boardX-cx, cy-boardY).

/* [Rendering] */
part = "exploded";   // [base, lid, both, assembled, exploded]
show_pcb = true;     // PCB + 18650 reference in 'assembled' / 'exploded'
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
base_th = 3;       // base plate thickness
pcb_clear = 0.5;   // gap between PCB edge and wall inner face
standoff_h = 2.5;  // PCB bottom height above base top
edge_r = 2.5;      // fillet radius for ALL outer edges of the body
batt_h = 21;       // PCB TOP to installed CELL TOP (user-specified)
ra01_h = 6;        // assumed module height above PCB
gps_h = 6.5;       // installed GPS height above PCB TOP

/* [Battery reference only — no retaining block on master] */
batt_d = 18.5;             // installed cell diameter, including wrapper
batt_x = 147.1 - pcb_cx;
batt_y = pcb_cy - 80.45;
/* [GPS antenna slide bracket] */
// Separate GPS antenna (not the U4 module): width 6 -> X, thickness 6.4 -> Y, length 19 -> Z.
// Inserted from the open -Z face toward the ceiling, so it stands upright in the lid print.
gps_ant_l = 19;
gps_ant_w = 6;
gps_ant_h = 6.4;
gps_ant_clear = 0.2;       // clearance per side in the XY cross-section
gps_ant_end_clear = 0.2;   // guide extends beyond the seated antenna's open end
gps_ant_rail = 1.6;
gps_ant_return = 1.6;      // full-height inward return on each side of the +X face
gps_ant_post_gap = 1;    // gap to the front (-Y) corner mounting post
gps_ant_recess_l = 6;      // +Z end recess length, along Z
gps_ant_recess_depth = 1;  // recess depth into the +X face, along X
gps_ant_recess_clear = 0.2;// fit clearance at the recess depth and end

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

/* [USB-C cutout — front/-Y wall] */
// U1 at (154.74401,124.704948), rotated 180; USB outline local X=3.12001..12.11999 -> centre 7.62.
usb_x = 154.74401 - 7.62 - pcb_cx;
usb_front_y = pcb_cy - (124.704948 + 1.95244);
usb_cut_w = 13;
usb_cut_h = 7;
usb_above_bot = 6.4;       // measured port centre above PCB BOTTOM

/* [SMA antenna — centre of left wall] */
ant_d = 6.5;

/* [Tripod mount — right/+X wall] */
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
usb_z     = pcb_bot_z + usb_above_bot;   // USB-C port centre Z
// Preserve shell/tripod height: battery clearance and centred tripod bore above Ra-01.
cavity    = max(batt_h + tri_gap, 2*(pcb_top_z + ra01_h + 4 + tri_pilot_d/2) - pcb_top_z - lid_th);
ceil_z    = pcb_top_z + cavity;      // lid inner ceiling
outer_h   = ceil_z + lid_th;         // overall height
post_len  = ceil_z - pcb_top_z;      // = cavity

tri_hole_z  = outer_h/2;                          // 1/4" bore, vertical centre of the right face
tri_box_bot = pcb_top_z + ra01_h + 2;             // 8 mm above PCB, clears GPS by 1.5 mm
ant_y       = 0;
ant_z       = outer_h/2;

batt_top_z = pcb_top_z + batt_h;
batt_axis_z = batt_top_z - batt_d/2;
// Print height is measured from the inner ceiling toward the open cover face.
gps_ant_slot_w = gps_ant_w + 2*gps_ant_clear;
gps_ant_slot_d = gps_ant_h + 2*gps_ant_clear;
gps_ant_bracket_h = gps_ant_l + gps_ant_end_clear;
// The cover wall is the -X guide face; no extra wall inside it.
gps_ant_x = -inner_l/2 + gps_ant_slot_w/2;
// Move down the left wall to just before the front corner post.
gps_ant_post_front = max(-inner_w/2 + (post_style == "corner" ? post_pad : 0),
                         -pcb_w/2 + mount_inset + post_d/2);
gps_ant_y = gps_ant_post_front + gps_ant_post_gap + gps_ant_slot_d/2 + gps_ant_rail;
gps_ant_fill_x = gps_ant_w/2-gps_ant_recess_depth+gps_ant_recess_clear;
gps_ant_fill_l = gps_ant_recess_l-gps_ant_recess_clear;
assert(2*gps_ant_return < gps_ant_slot_d, "Keep the centre of the +X face open");
assert(gps_ant_return > gps_ant_clear, "Return walls must overlap the antenna face");
assert(gps_ant_recess_clear < min(gps_ant_recess_l,gps_ant_recess_depth),
       "Recess filler must enter the antenna recess");
assert(ceil_z-gps_ant_bracket_h > pcb_top_z, "Socket must stop above the PCB");
assert(gps_ant_y+gps_ant_slot_d/2+gps_ant_rail < ant_y-ant_d/2,
       "Socket must clear the SMA opening");

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

// Self-tapping posts (ceiling -> PCB top) in the corners; post_style:
//   "corner" = post in a solid corner pad (rounded cavity edge)
//   "rib"    = cylinder + two slim ribs to the adjacent walls
//   "plain"  = bare cylinder
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

// Side walls, full-height +X returns and recess filler, all starting on the ceiling/print floor.
module gps_antenna_bracket() {
    translate([gps_ant_x,gps_ant_y,ceil_z]) mirror([0,0,1]) {
        for (sy=[-1,1]) {
            // Side wall, fused into the -X housing wall.
            translate([-gps_ant_slot_w/2-eps,
                       sy < 0 ? -gps_ant_slot_d/2-gps_ant_rail : gps_ant_slot_d/2,
                       -eps])
                cube([gps_ant_slot_w+gps_ant_rail+eps,gps_ant_rail,gps_ant_bracket_h+eps]);
            // L-shaped return, fused along the full-height edge of the side wall.
            translate([gps_ant_slot_w/2,
                       sy < 0 ? -gps_ant_slot_d/2-eps : gps_ant_slot_d/2-gps_ant_return,
                       -eps])
                cube([gps_ant_rail,gps_ant_return+eps,gps_ant_bracket_h+eps]);
        }
        // +Z-end recess filler across both walls: nominal Z 6 / X 1 mm, less 0.2 fit -> 5.8 / 0.8.
        translate([gps_ant_fill_x,-gps_ant_slot_d/2-eps,-eps])
            cube([gps_ant_slot_w/2+gps_ant_rail-gps_ant_fill_x,
                  gps_ant_slot_d+2*eps,gps_ant_fill_l+eps]);
    }
}

module gps_antenna_ref() {
    if ($preview)
        color([0.8,0.65,0.3,0.7]) difference() {
            translate([gps_ant_x-gps_ant_w/2,gps_ant_y-gps_ant_h/2,ceil_z-gps_ant_l])
                cube([gps_ant_w,gps_ant_h,gps_ant_l]);
            translate([gps_ant_x+gps_ant_w/2-gps_ant_recess_depth,
                       gps_ant_y-gps_ant_h/2-eps,ceil_z-gps_ant_recess_l])
                cube([gps_ant_recess_depth+eps,gps_ant_h+2*eps,gps_ant_recess_l+eps]);
        }
}

// ---------- LID (본체 하우징): fully-rounded shell, open bottom ----------
module lid() {
    difference() {
        union() {
            difference() {
                rbox_all(outer_l, outer_w, outer_h, outer_r, edge_r);          // fully-rounded body
                translate([0,0,-eps]) rbox(inner_l, inner_w, ceil_z+eps, inner_r); // cavity
            }
            posts();
            tripod_box();
            gps_antenna_bracket();
        }

        // post pilot bores
        for (p=mount_pos) translate([p[0],p[1],pcb_top_z-eps]) cylinder(h=post_pilot_depth, d=post_pilot_d);

        // USB-C opening (-Y wall)
        translate([usb_x, -inner_w/2+eps, usb_z])
            rotate([90,0,0])
                linear_extrude(wall+pcb_clear+1+eps) rrect(usb_cut_w, usb_cut_h, 1);
        // SMA antenna bulkhead, centre of the left wall
        xcyl(-outer_l/2-eps, ant_y, ant_z, wall+pcb_clear+1+2*eps, ant_d);
        // tripod self-tap pilot (+X wall, blind, horizontal into the box)
        xcyl(outer_l/2 - tri_depth - eps, tri_y, tri_hole_z, tri_depth + 2*eps, tri_pilot_d);
    }
}

// ---------- BASE (바닥판): flat plate plugging the bottom opening ----------
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

// ---------- REFERENCE: F5 preview only ($preview), never in F6 render / STL export ----------
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
        // U4 GPS body: local bounds [-6.6,-2.03]..[6.6,14.22], no rotation.
        color([0.15,0.4,0.8,0.5])
            translate([184.912-pcb_cx-6.6,pcb_cy-94.489-14.22,pcb_top_z])
                cube([13.2,16.25,gps_h]);
        color([0.55,0.55,0.6,0.6])
            translate([usb_x,usb_front_y+3.25,usb_z]) cube([9,6.5,3.26],center=true);

    }
}

// ---------- RENDER ----------
if (part=="base") base();
else if (part=="lid")
    translate([0,0,outer_h]) rotate([180,0,0]) lid();          // flipped ceiling-down to print
else if (part=="both") {
    translate([-(outer_l/2+8),0,0]) base();
    translate([ (outer_l/2+8),0,outer_h]) rotate([180,0,0]) lid();
}
else if (part=="assembled") {
    color("lightblue") base();
    color("khaki", 0.85) lid();
    if (show_pcb) gps_antenna_ref();
    if (show_pcb) pcb_ref();
}
else if (part=="exploded") {
    // stacked bottom -> top along Z: base, PCB, lid
    color("lightblue") base();
    if (show_pcb) translate([0,0,explode]) pcb_ref();
    color("khaki", 0.85) translate([0,0,2*explode]) lid();
    if (show_pcb) translate([0,0,2*explode]) gps_antenna_ref();
}
