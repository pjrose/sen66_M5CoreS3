# SEN66 AQM Enclosure v3.1

- Eight full-diameter insert pilots now pass through the rear; fit coupons also
  use through holes. Check hardware engagement and rear protrusion.
- Divider shifted 0.7 mm left; all 22 remaining capsule vents are unobstructed.
- Removed the lone right-side vent and half-moon QT cable notch.
- Main right opening is still 45 mm wide, with its bottom at the Core rear
  plane (Z=13.8 mm). The QT route now drops inside the side gap.
- Service-window sides extend straight to the faceplate, without pointed upper
  returns. Their lower corners retain 3 mm radii.
- New insert/vent/window/route checks and a sixth, right-side review render.

The published `enclosure-v3` release and ZIP are unchanged. This revision
has regenerated source, STEP, STL, Blender, render and validation files in the
same directory. The faceplate and three-keyhole mounting pattern are unchanged.

## Previous v3 Release

Compact, serviceable PLA enclosure for the supplied CoreS3 housing, SEN66 and
Adafruit 6331 adapter. This release changes enclosure files only, not firmware.

- 108 x 76 x 33.8 mm body, 34.4 mm including lettering.
- Centered device layout, smooth labeled bezel and rounded ventilation/access openings.
- Positive upper/lower device stops and a flat four-insert M2.5 adapter mesa.
- Three aligned wall keyholes at X=22, 54, 86 mm and Y=61.25 mm from the bottom.
  Use the centre for one-screw mounting or the outer pair for resistance to rotation.
- Four countersunk M3 front screws; no nuts, feet or unnecessary rear slots.
- All five CAD renders, print STLs, editable STEP, parametric sources, Blender
  scene, insert coupons, validation results and assembly guide included.

**First-fit prototype:** automated geometry, support-contact and screw-head
clearance checks pass. Physical fit, thermal behaviour and wall-load retention
have not been tested. Confirm the provisional M2.5 insert size (3.5 mm OD x 4 mm)
and your screw/plug dimensions before printing.

See [the assembly guide](https://github.com/pjrose/sen66_M5CoreS3/blob/main/hardware/enclosure_v3/README.md)
and [renders](https://github.com/pjrose/sen66_M5CoreS3/tree/main/hardware/enclosure_v3/renders).
