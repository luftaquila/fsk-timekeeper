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
// Wall features (SENSOR variant — sensor I/O only; the light connector is not used):
//   - left  (-X): 1x Molex Mini-Fit Jr (J1 sensor +12V/SENSE). J2 (light GN/RD) opening is blanked off
//                 (solid wall). SMA antenna stays between the connector positions.
//   - right (+X): 1/4" tripod mount     (Ra-01 IPEX -> pigtail across the box -> antenna on the left)
//   - front (-Y): USB-C  (nRF52840 SuperMini) + 6mm toggle switch bushing (-Y wall, left)
//   - back  (+Y): blank
//
// Coordinate frame: PCB centred at XY origin; Z=0 at the outer bottom of the assembly.

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

/* [Shell] */
wall = 2.5;        // side wall thickness
lid_th = 2;        // lid ceiling thickness
base_th = 3;       // base plate thickness (>= edge_r so its bottom edge can be filleted)
pcb_clear = 0.5;   // gap between PCB edge and wall inner face
standoff_h = 2.5;  // PCB bottom height above base top (clears bottom-side 0603 caps ~0.5)
edge_r = 2.5;      // fillet radius for ALL outer edges of the body
batt_h = 21;       // *** ASSUMPTION: 18650 holder+cell height above PCB. MEASURE & adjust. ***
ra01_h = 6;        // *** ASSUMPTION: Ra-01 module height above PCB (limits how low the tripod box reaches). ***

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

/* [Molex Mini-Fit Jr cutouts — left/-X wall] */
// KiCad +Y is screen-DOWN, OpenSCAD +Y is UP  ->  model_Y = 139.5 - boardY  (Y is flipped).
// Opening centre = connector WIDTH centre (pin centroid), NOT the footprint origin: the 2 circuits sit
// +2.1mm (local-X) from the origin -> J1 boardY 140.65 -> 139.5-(140.65-2.1) = 0.95; J2 -> -11.25.
molex_y = [0.95, -11.25];   // connector opening Y centres (model)
molex_cut_w = 11;          // opening width (Y) — clears the 9.6mm shroud (Molex SD-5569) + margin
molex_cut_h = 11;          // opening height above PCB (Z) — 9.6mm shroud + right-angle standoff (sensor variant: lowered 1mm, latch slot drops with it but keeps its height)
molex_latch_w = 4;         // latch slot width — clears the 3.0mm engaging tongue (Molex SD-5557 ".12[3.0]"); the 5.4mm is the external release handle, not through the wall (sensor variant: widened 0.5mm)
molex_latch_h = 4.5;       // latch slot extra height — must clear the latch at MAX deflection during mating (rest ~2.5mm + flex), not just at rest; SD shows rest only, so kept generous (sensor variant: net -0.5mm from baseline 5)

/* [USB-C cutout — -Y wall] */
// USB-C sits on the SuperMini's short end (board Y~167, at the -Y wall). Its X centre is the MODULE-WIDTH
// centre, NOT the footprint origin (origin = a corner pad): end pads span board X 140.47..155.71 ->
// centre 148.09 -> model 6.09. Port CENTRE height is measured 6.4mm above the PCB bottom face.
usb_x = 6.1;          // X centre of the USB-C port (model)
usb_cut_w = 13;       // opening width (X) incl. cable-boot clearance
usb_cut_h = 7;        // opening height (Z)
usb_above_bot = 6.4;  // port CENTRE height above the PCB bottom face (measured)

/* [Toggle switch bushing hole — -Y wall, LEFT (-X), clear of the Molex] */
// 6mm bushing toggle (MTS-/STM- 6mm series). The switch body needs a sw_cube clearance box
// reaching inward from the -Y wall; it is kept EAST (+X) of the Molex courtyard, which reaches
// model X -27.45 (J1 Y -4.35..+6.25, J2 Y -16.55..-5.95, Z 7.1..18), and ABOVE the PCB.
sw_d = 6;          // bushing through-hole (6mm toggle)
sw_cube = 14;      // required interior body-clearance cube (edge length)
sw_x = -15;        // hole X centre on the -Y wall (moved +5mm toward the tripod/+X side); cube -X edge -22 clears Molex (+X edge -27.45)
sw_z = 19;         // hole Z centre: cube spans z 12..26 -> above PCB + low parts (incl. L1 ~z10), below ceiling 37.6

/* [SMA antenna hole — -X (left) wall] */
// sensor variant (J2/light blanked off): bore centred above box mid-height (Z), and in Y midway between
// the -Y end of the J1 (sensor) opening and the -Y inner wall, nudged toward the connector, into the
// wall freed where J2 used to be.  (ant_y/ant_z are computed in the derived section, where inner_w exists.)
ant_d = 6.5;                             // SMA bulkhead through-hole

/* [Tripod mount — +X (right) wall, ceiling-integrated box (prints support-free)] */
// On the right wall (antenna side). A box hangs from the ceiling (fused to ceiling + wall; printed
// ceiling-down it is a solid column off the bed -> no support) and extends DOWN to the 1/4" bore,
// which sits at the vertical CENTRE of the face. At Y=0 the battery is absent (cell ends ~Y+8, the
// +X contact is at Y+14..+21), so only the low Ra-01 module limits how far down the box reaches.
tri_box_w = 12;    // box width (Y)
tri_y = -1.5;      // box Y centre — shifted -Y off the face centre so it clears the battery holder body (-Y edge ~ +6.75)
tri_box_d = 10;    // box depth into the cavity (X from the wall)
tri_pilot_d = 5.7; // 1/4"-20 self-tap pilot in plastic (was 5.4 — too tight to thread in)
tri_depth = 8;     // thread engagement depth from the outer wall face
tri_gap = 2;       // clearance between the box bottom and the Molex envelope

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
tri_box_bot = pcb_top_z + ra01_h + 2;             // box hangs from the ceiling down to here (clears the Ra-01 module)
ant_y       = ((molex_y[0] - molex_cut_w/2) + (-inner_w/2)) / 2 + 1;   // midway: sensor-hole -Y end <-> -Y inner wall, +1mm toward the connector
ant_z       = outer_h/2 + 8;                      // sensor variant: above box mid-height

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

// tripod mount: a rectangular box fused to the ceiling and the -X wall, hanging down to just
// above the Molex. Printed ceiling-down ('lid' orientation) it is a solid column off the bed,
// so it needs no support. The 1/4" bore is a short horizontal blind hole into it.
module tripod_box() {
    translate([inner_l/2 - tri_box_d, tri_y - tri_box_w/2, tri_box_bot])
        cube([tri_box_d, tri_box_w, ceil_z - tri_box_bot + eps]);
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
        }

        // post pilot bores
        for (p=mount_pos) translate([p[0],p[1],pcb_top_z-eps]) cylinder(h=post_pilot_depth, d=post_pilot_d);

        // Molex cutouts (-X wall): main opening + latch slot
        // sensor variant: J2 (light) is blanked off — only J1 (sensor) is cut.
        for (cy = [molex_y[0]]) {
            translate([-outer_l/2-eps, cy-molex_cut_w/2, pcb_top_z-0.5])
                cube([wall+pcb_clear+1+eps, molex_cut_w, molex_cut_h+0.5]);
            translate([-outer_l/2-eps, cy-molex_latch_w/2, pcb_top_z-0.5+molex_cut_h])
                cube([wall+pcb_clear+1+eps, molex_latch_w, molex_latch_h]);
        }
        // USB-C (-Y wall) — opening with r=1 rounded corners, centred at usb_z
        translate([usb_x, -inner_w/2+eps, usb_z])
            rotate([90,0,0])
                linear_extrude(wall+pcb_clear+1+eps) rrect(usb_cut_w, usb_cut_h, 1);
        // toggle switch bushing (-Y wall, left, clear of the Molex)
        translate([sw_x, -inner_w/2+eps, sw_z]) rotate([90,0,0])
            cylinder(h=wall+pcb_clear+1+eps, d=sw_d);
        // SMA antenna bulkhead (-X / left wall, raised above the Molex)
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
        bx=-0.065; by=17.58; bt=pcb_top_z+batt_h;   // BT1 holder centre / top (board 121.92 -> 139.5-121.92 = +17.58)
        color([0.75,0.75,0.78,0.45]) {
            translate([bx,by,bt-9.25]) rotate([0,90,0]) cylinder(h=65,d=18.5,center=true);    // 18650 cell
            translate([bx,by,(pcb_top_z+bt)/2]) cube([78.06,21.66,bt-pcb_top_z],center=true);  // holder body
            translate([bx-41.5,by,(pcb_top_z+bt)/2]) cube([4.9,7.5,bt-pcb_top_z],center=true);  // end contact -X
            translate([bx+41.5,by,(pcb_top_z+bt)/2]) cube([4.9,7.5,bt-pcb_top_z],center=true);  // end contact +X
        }
        color([0.2,0.2,0.28,0.5]) translate([28.33,-7.92,pcb_top_z+ra01_h/2]) cube([18.5,18,ra01_h],center=true);  // Ra-01 module (board 147.42 -> 139.5-147.42 = -7.92)
        // Molex 5569 right-angle body envelope (courtyard) — reaches model X -27.45; ±5.3 in Y about the opening centre
        color([0.85,0.5,0.15,0.4]) for (cy=molex_y)
            translate([-inner_l/2, cy-5.3, pcb_top_z]) cube([(-27.45)+inner_l/2, 10.6, 11]);
        // USB-C receptacle (SuperMini short end) at the -Y wall — centred (usb_x, usb_z)
        color([0.55,0.55,0.6,0.6]) translate([usb_x, -inner_w/2+3.5, usb_z]) cube([8.94,7,3.26],center=true);
        // toggle switch body clearance cube (must not foul anything) — sits at the -Y wall, reaching inward
        %translate([sw_x, -inner_w/2 + sw_cube/2, sw_z]) cube(sw_cube, center=true);
    }
}

// ============================================================
//  RENDER
// ============================================================
if (part=="base") base();
else if (part=="lid")
    translate([0,0,outer_h]) rotate([180,0,0]) lid();          // flipped onto its ceiling for printing
else if (part=="both") {
    translate([-(outer_l/2+8),0,0]) base();
    translate([ (outer_l/2+8),0,outer_h]) rotate([180,0,0]) lid();
}
else if (part=="assembled") {
    color("lightblue") base();
    color("khaki", 0.85) lid();
    if (show_pcb) pcb_ref();
}
else if (part=="exploded") {
    // stacked bottom -> top along Z: base, PCB, lid
    color("lightblue") base();
    if (show_pcb) translate([0,0,explode]) pcb_ref();
    color("khaki", 0.85) translate([0,0,2*explode]) lid();
}
