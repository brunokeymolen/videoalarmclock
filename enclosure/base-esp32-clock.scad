// Smoothness of curves
$fn = 64; 

// --- UPDATED BASE BLOCK (Horizontal) ---
length1 = 90;   
width1 = 50;    
height1 = 10;   
r1 = 5; // Corner radius

module base_block() {
    minkowski() {
        cube([length1 - 2*r1, width1 - 2*r1, height1], center=true);
        cylinder(r=r1, h=0.0001, center=true);
    }
}

// --- PERPENDICULAR BLOCK (Straight edges) ---
lengthEsp = 86.80;
widthEsp = 15.5;
heightEsp = 86.30;

module perp_block() {
    cube([lengthEsp, widthEsp, heightEsp], center=true);
}

// --- ASSEMBLY ---
difference() {
    // 1. Main body to keep
    base_block();
    
    // 2. Body to cut out (lowered and tilted 15 degrees)
    translate([0, 0, (height1 / 2) + (heightEsp / 2) - 6]) //  a -8 build is a bit more sturdy
    rotate([15, 0, 0])
    perp_block();
}
