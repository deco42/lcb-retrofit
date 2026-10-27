// End cap for a cut DIN rail TS35 x 7.5 (EN 60715)

rail_t = 1.0;    // rail thickness
clear  = 0.2;    // clearance between rail and cap
wall   = 1.6;
depth  = 10;     // how far the cap reaches over the rail
lid    = 1.6;
$fn = 32;

module profile() {
    translate([-13.5, 0]) square([27, rail_t]);                                // base
    for (s = [-1, 1]) {
        translate([s < 0 ? -13.5 : 13.5 - rail_t, 0]) square([rail_t, 7.5]);   // side wall
        translate([s < 0 ? -17.5 : 13.5, 7.5 - rail_t]) square([4, rail_t]);   // flange
    }
}

module open_bottom() { intersection() { children(); translate([-30, 0]) square([60, 20]); } }

linear_extrude(lid) open_bottom() offset(r = wall) profile();
translate([0, 0, lid - 0.01]) linear_extrude(depth)
    open_bottom() difference() { offset(r = wall) profile(); offset(delta = clear) profile(); }
