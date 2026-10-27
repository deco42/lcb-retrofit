// DIN rail holder (TS35) for the DB37 terminal adapter Delock 65320 ("DB37MT")

pcb_h      = 69.3;   // PCB length along the rail
pcb_t      = 1.6;    // PCB thickness
dsub_x     = 49.5;   // measure: left PCB edge to start of the black connector body
pcb_x_end  = 62.0;   // measure: left PCB edge to PCB end under the connector
foam_t     = 4.0;    // factory foam under the PCB
squeeze    = 0.5;    // foam compression under the lip

/* [Rail] */
rail_x     = 27.5;   // rail centre under the PCB
rail_w     = 35.0;
rail_gap   = 1.6;    // gap for the rail edge

/* [Holder] */
base_t     = 3.5;
wall       = 3.0;
fit        = 0.25;   // clearance around the PCB
lip        = 2.0;    // overlap of the left lip over the PCB
tie_x      = 56.0;   // cable tie over the connector body
tie_w      = 4.4;    // groove for cable ties up to 3.6 mm wide
lip_y0     = 27;     // left lip only between the terminal rows
lip_y1     = 47;
tab        = 8.0;    // tab for the clamping screw
$fn = 40;

z0   = base_t + foam_t - squeeze;   // PCB bottom
ztop = z0 + pcb_t;                  // PCB top
x0   = -wall;                       // left edge of the holder
x1   = pcb_x_end;                   // right edge of the holder
y0   = -fit - wall;
y1   = pcb_h + fit + wall;

module holder() {
    difference() {
        union() {
            translate([x0, y0, 0]) cube([x1 - x0, y1 - y0, base_t]);                 // base plate
            translate([x0, y1, 0]) cube([25 - x0, tab, base_t]);                     // tab for the clamping screw
            translate([rail_x - rail_w / 2 + 2.5, y1 + tab / 2, 0])                  // screw boss
                cylinder(d = 7, h = base_t + 4);
            // left wall up to just below the PCB top, higher in the middle with the lip
            translate([x0, y0, 0]) cube([wall - fit, y1 - y0, ztop - 0.2]);
            hull() {                                                                 // lip, chamfered towards -y
                translate([x0, lip_y0, 0]) cube([wall - fit + lip, lip_y1 - lip_y0, ztop + fit + 1.4]);
                translate([x0, lip_y0 - lip - 1, 0]) cube([wall - fit, 0.01, ztop + fit + 1.4]);
            }
            // end walls, only up to the connector
            translate([x0, y0, 0]) cube([dsub_x - 1.5 - x0, wall, ztop - 0.2]);      // y start (on the bed when printing)
            translate([x0, pcb_h + fit, 0]) cube([dsub_x - 1.5 - x0, wall, ztop - 0.2]);
            // rail hooks left and right, over the full length
            for (s = [-1, 1]) {
                xe = rail_x + s * rail_w / 2;                    // rail edge
                translate([s < 0 ? xe - 0.3 - 2.5 : xe + 0.3, y0, -(rail_gap + 2)])   // vertical
                    cube([2.5, y1 + tab - y0, rail_gap + 2 + 0.01]);
                translate([s < 0 ? xe - 0.3 : xe - 2.5 + 0.3, y0, -(rail_gap + 2)])   // lip under the rail
                    cube([2.5, y1 + tab - y0, 2]);
            }
        }
        // space for foam and PCB
        translate([-fit, -fit, base_t]) cube([pcb_x_end + fit, pcb_h + 2 * fit, ztop + fit - base_t]);
        // grooves for the cable tie so it cannot slip
        translate([tie_x - tie_w / 2, y0 - 1, -1]) cube([tie_w, 1.8, base_t + 2]);
        translate([tie_x - tie_w / 2, y1 - 0.8, -1]) cube([tie_w, 1.8, base_t + 2]);
        translate([tie_x - tie_w / 2, y0 - 1, -1]) cube([tie_w, y1 - y0 + 2, 1.8]);
        // clamping screw M3, self-tapping, presses on the left rail edge
        translate([rail_x - rail_w / 2 + 2.5, y1 + tab / 2, -5]) cylinder(d = 2.5, h = 20);
    }
}

translate([0, 0, -y0]) rotate([90, 0, 0]) holder();
