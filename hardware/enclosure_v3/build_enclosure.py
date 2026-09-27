"""Exact-solid enclosure in mm: X right, Y up on wall, Z toward the user.

Only print/ contains slicer-ready parts. Reference meshes are fit checks, not
printable electronics. Keep old revisions intact when regenerating this one.
"""
from pathlib import Path
import argparse
import json
import math
import cadquery as cq
import trimesh
import numpy as np

ROOT = Path(__file__).resolve().parent
P = json.loads((ROOT / 'parameters.json').read_text())
for folder in ('print', 'assembly', 'reference', 'renders'):
    (ROOT / folder).mkdir(exist_ok=True)
parts = {}


def box(w, h, d, x, y, z):
    return cq.Workplane('XY').box(w, h, d, centered=(True, True, False)).translate((x, y, z))


def rounded(w, h, d, r, x, y, z):
    return box(w, h, d, x, y, z).edges('|Z').fillet(r)


def cyl(r, d, x, y, z):
    return cq.Workplane('XY').circle(r).extrude(d).translate((x, y, z))


def slot(length, width, d, x, y, z, angle=0):
    return cq.Workplane('XY').slot2D(length, width, angle).extrude(d).translate((x, y, z))


def socket(body, x, y, top, family='m3'):
    pilot, length = P[family + '_insert_pilot'], P[family + '_insert_length']
    assert top - length - .5 >= 1.2, 'Insufficient floor beneath insert'
    body = body.cut(cyl(pilot / 2, length + .5, x, y, top - length - .5))
    return body.cut(cq.Solid.makeCone(pilot / 2, pilot / 2 + .3, .5, cq.Vector(x, y, top - .5)))


def countersink(body, x, y, top, thickness):
    r, R = P['m3_clearance'] / 2, P['m3_countersink_diameter'] / 2
    body = body.cut(cyl(r, thickness + 2, x, y, top - thickness - 1))
    return body.cut(cq.Solid.makeCone(r, R, R - r, cq.Vector(x, y, top - (R - r))))


def vent(plane, center, z0, z1, width, depth, offset):
    return (cq.Workplane(plane).center(center, (z0 + z1) / 2)
            .slot2D(z1 - z0, width, 90).extrude(depth).translate(offset))


def service_window(plane, center, width, z0, z1, depth, offset):
    # Rounded at all four corners. Its top touches the shell joint: the
    # separately printed flat bezel supplies the roof, not a long bridge.
    solid = rounded(width, z1 - z0, depth, P['service_corner_radius'], center, (z0 + z1) / 2, 0)
    if plane == 'YZ':
        solid = solid.rotate((0, 0, 0), (1, 1, 1), 120)
    else:
        solid = solid.rotate((0, 0, 0), (1, 0, 0), 90)
    return solid.translate(offset)


def export_part(name, shape):
    assert shape.val().isValid(), name + ' invalid BREP'
    assert len(shape.solids().vals()) == 1, name + ' disconnected solids'
    cq.exporters.export(shape, str(ROOT / 'assembly' / (name + '.step')))
    cq.exporters.export(shape, str(ROOT / 'assembly' / (name + '.stl')), tolerance=.03, angularTolerance=.10)
    bb = shape.val().BoundingBox()
    flat = shape.translate((-bb.xmin, -bb.ymin, -bb.zmin))
    cq.exporters.export(flat, str(ROOT / 'print' / (name + '.stl')), tolerance=.03, angularTolerance=.10)
    parts[name] = shape


W, H, B = P['width'], P['height'], P['base_thickness']
wall, face_t = P['wall_thickness'], P['face_thickness']
cx, cy = P['core_center']
sx, sy = P['sensor_center']
cw, ch, cd = P['core_width'], P['core_height'], P['core_depth']
sr = P['sensor_rear_z']
face = sr + P['sensor_depth']
cr = face - cd
pad, clearance = P['compressed_pad'], P['clearance']
joint, front = face + pad, face + pad + face_t
inset = P['corner_screw_inset']
face_bolts = [(x, y) for x in (inset, W - inset) for y in (inset, H - inset)]
px, py, pz = P['pcb_origin']
board_bolts = [(px + x, py + y) for x in (2.54, 22.86) for y in (2.54, 15.24)]
assert cy == sy == H / 2, 'Device centre lines must match'

base = rounded(W, H, joint, 4, W / 2, H / 2, 0)
base = base.cut(rounded(W - 2 * wall, H - 2 * wall, joint - B + 1, 4 - wall, W / 2, H / 2, B))
for x, y in face_bolts:
    base = socket(base.union(cyl(4.5, joint - B, x, y, B)), x, y, joint)

base = base.cut(service_window('YZ', cy + 1.5, 45, 11, joint, wall + 2, (W - wall - 1, 0, 0)))
base = base.cut(vent('YZ', cy - 3, 7, 13, 5.2, wall + 2, (W - wall - 1, 0, 0)))
base = base.cut(service_window('XZ', cx - 1, 40, 15.5, joint, wall + 2, (0, H + 1, 0)))
for y in range(15, int(H - 10), 7):
    base = base.cut(vent('YZ', y, 8, 24, 3.4, wall + 2, (-1, 0, 0)))
for y in (10,):
    base = base.cut(vent('YZ', y, 8, 24, 3.4, wall + 2, (W - wall - 1, 0, 0)))
for x in range(18, 91, 8):
    base = base.cut(vent('XZ', x, 8, 24, 3.4, wall + 2, (0, wall + 1, 0)))
for x in (18, 26, 34, 42):
    base = base.cut(vent('XZ', x, 8, 24, 3.4, wall + 2, (0, H + 1, 0)))

divider = box(1.6, H - 2 * wall, joint - B, 40, H / 2, B)
divider = divider.cut(vent('YZ', sy - 3, 5.8, 22, 14, 4, (38, 0, 0)))
base = base.union(divider)

core_supports = []
for x in (cx - 23, cx + 23):
    for y in (cy - 23, cy + 23):
        # The upper-left support remains under the Core's outer rear rim,
        # but is narrower and shifted left to clear the centre wall screw.
        is_upper_left = x < cx and y > cy
        foot_x = x - 2.65 if is_upper_left else x
        foot_w = 2.4 if is_upper_left else 6
        foot = rounded(foot_w, 6, cr - pad - B, .6 if is_upper_left else 1, foot_x, y, B)
        core_supports.append(foot)
        base = base.union(foot)
        outer_x = cx + math.copysign(cw / 2 + clearance + 1.1, x - cx)
        base = base.union(box(2.2, 6, cr + 4 - B, outer_x, y, B))
        base = base.union(box(4.4, 6, cr - pad - B, (foot_x + outer_x) / 2, y, B))
for x in (sx - 9, sx + 9):
    for y in (sy - 23, sy + 23):
        base = base.union(rounded(4, 4, sr - pad - B, .7, x, y, B))
for side in (-1, 1):
    x = sx + side * (P['sensor_width'] / 2 + clearance + 1)
    for y in (sy - 21, sy + 21):
        base = base.union(box(2, 6, sr + 7 - B, x, y, B))
        base = base.union(box(5, 6, sr - pad - B, x - side * 2, y, B))

# End walls give positive Y retention rather than relying on friction from
# the bezel. Short Core tabs near its corners leave SD/reset fully exposed.
stops = {'core': [], 'sensor': []}
for sign in (-1, 1):
    for x in (cx - 23, cx + 23):
        stop = rounded(6, 3, cr + 5 - B, .6, x, cy + sign * (ch / 2 + clearance + 1.5), B)
        base = base.union(stop)
        stops['core'].append(stop)
    stop = rounded(12, 3, sr + 6 - B, .6, sx, sy + sign * (P['sensor_length'] / 2 + clearance + 1.5), B)
    base = base.union(stop)
    stops['sensor'].append(stop)

# Continuous flat PCB mesa, four blind M2.5 insert sockets. No fingers,
# cantilevers, nuts, or clamp plates are needed for the adapter.
base = base.union(rounded(28.4, 20.78, pz - B, 1.5, px + 12.7, py + 8.89, B))
for x, y in board_bolts:
    base = socket(base, x, y, pz, 'm25')

# Three alternative keyholes share a high horizontal row. The outer holes
# are symmetric about the case centre, with the left under the SEN centre.
# With the current stops, a 7.5 mm head has 0.5 mm clearance above this row.
head_sweeps = []
head_entries = []
for x, y in P['wall_mount_centres']:
    drop = P['wall_mount_drop']
    base = base.cut(cyl(P['wall_mount_entry_diameter'] / 2, B + 2, x, y - drop, -1))
    base = base.cut(slot(drop + P['wall_mount_track_width'], P['wall_mount_track_width'], B + 2, x, y - drop / 2, -1, 90))
    head_sweeps.append(slot(drop + P['wall_screw_head_diameter'], P['wall_screw_head_diameter'],
        P['wall_screw_head_height'], x, y - drop / 2, B + P['wall_screw_running_clearance'], 90))
    head_entries.append(cyl(P['wall_screw_head_diameter'] / 2,
        B + P['wall_screw_running_clearance'] + P['wall_screw_head_height'] + 1, x, y - drop, -1))
export_part('01_vented_shell', base)

bezel = rounded(W, H, face_t, 4, W / 2, H / 2, joint)
bezel = bezel.cut(rounded(*P['core_window'], face_t + 2, 3.5, cx, cy, joint - 1))
bezel = bezel.cut(rounded(*P['sensor_window'], face_t + 2, 1.2, sx, sy, joint - 1))
bezel = bezel.faces('>Z').edges().chamfer(.5)
for x, y in face_bolts:
    bezel = countersink(bezel, x, y, front, face_t)


def label(text, size, x, y, angle=0):
    font = Path('C:/Windows/Fonts/arialbd.ttf')
    obj = cq.Workplane('XY').text(text, size, P['emboss_height'] + .02, combine=False,
        fontPath=str(font) if font.exists() else None, font='Arial', kind='bold', halign='center', valign='center')
    return obj.rotate((0, 0, 0), (0, 0, 1), angle).translate((x, y, front - .02))


markings = [label('USB', 4.1, 104, cy, 90), label('I2C', 4.1, 104, cy + 14, 90),
            label('SD', 4.1, cx + 6.6, H - 5), label('SEN66 AQM', 5.2, cx, 5.5)]
# Clockwise arc and a tangent arrowhead deliberately overlap. Validate the
# icon by itself so a disconnected tip cannot hide in the bezel union.
angles = np.linspace(math.radians(-45), math.radians(-315), 64)
outline = [(2.2 * math.cos(a), 2.2 * math.sin(a)) for a in angles]
outline += [(1.4 * math.cos(a), 1.4 * math.sin(a)) for a in angles[::-1]]
arrow = cq.Workplane('XY').polyline(outline).close().extrude(P['emboss_height'] + .02)
a = angles[-1]
n = np.array([math.cos(a), math.sin(a)])
t = np.array([math.sin(a), -math.cos(a)])
end = 1.8 * n
tip, back = end + 1.35 * t, end - .65 * t
triangle = [tuple(tip), tuple(back + 1.05 * n), tuple(back - 1.05 * n)]
arrow = arrow.union(cq.Workplane('XY').polyline(triangle).close().extrude(P['emboss_height'] + .02))
assert len(arrow.solids().vals()) == 1, 'Reset arrowhead disconnected'
markings.append(arrow.translate((cx - 14.7, H - 5, front - .02)))
ring = cyl(2.1, P['emboss_height'] + .02, 0, 0, 0).cut(cyl(1.4, P['emboss_height'] + 1, 0, 0, -.1))
ring = ring.cut(box(1.7, 3, 2, 0, 2, 0)).union(box(.7, 2.8, P['emboss_height'] + .02, 0, 1, 0))
markings.append(ring.translate((104, cy - 13, front - .02)))
for m in markings:
    bezel = bezel.union(m)
cq.exporters.export(cq.Compound.makeCompound([s for m in markings for s in m.solids().vals()]),
                    str(ROOT / 'reference' / 'emboss_markings.stl'), tolerance=.02)
export_part('02_front_bezel', bezel)

for name, family, diameters in [('03_M3_insert_test', 'm3', [4.0, 4.1, 4.2, 4.3, 4.4]),
                               ('04_M25_insert_test', 'm25', [3.0, 3.1, 3.2, 3.3, 3.4])]:
    coupon = rounded(60, 13, 8, 2, 30, 6.5, 0).cut(cyl(1.5, 10, 0, 6.5, -1))
    for i, d in enumerate(diameters):
        coupon = coupon.cut(cyl(d / 2, P[family + '_insert_length'] + .5, 8 + i * 11, 6.5, 8 - P[family + '_insert_length'] - .5))
    export_part(name, coupon)

names = ['01_vented_shell', '02_front_bezel']
assembly = cq.Assembly(name='Core_air_compact_enclosure_v3')
for name in names:
    assembly.add(parts[name], name=name)
assembly.export(str(ROOT / 'enclosure.step'))

report = {'parameters': P, 'derived': {'core_rear_z': cr, 'device_front_z': face,
    'bezel_front_z': front, 'wall_to_front_mm': front, 'max_depth_with_labels_mm': front + P['emboss_height'],
    'adapter_to_core_gap_mm': cr - (pz + 5.82), 'adapter_hole_pitch_mm': [20.32, 12.70],
    'insert_floor_mm': pz - P['m25_insert_length'] - .5},
    'parts': {}, 'intersections': [], 'access_intersections': [], 'wall_head_intersections': []}
for name, shape in parts.items():
    m = trimesh.load(ROOT / 'print' / (name + '.stl'))
    r = {'watertight': bool(m.is_watertight), 'positive_volume': bool(m.volume > 0),
         'connected_components': len(m.split()), 'size_mm': m.extents.tolist(),
         'volume_cm3': float(m.volume / 1000), 'brep_valid': shape.val().isValid()}
    report['parts'][name] = r
    assert r['watertight'] and r['positive_volume'] and r['connected_components'] == 1, (name, r)
overlap = base.intersect(bezel).val().Volume()
assert overlap < .01, ('Shell/bezel interference', overlap)

keepouts = {
    'USB_plug_and_grip': box(P['usb_service_depth'], 14, 12, cx + 27 + P['usb_service_depth'] / 2, cy, cr + 2.4),
    'Grove_plug': box(14, 11, 8, cx + 34, cy + 14, cr + 3.4),
    'microSD_extraction': box(16, 25, 8, cx + 6.6, cy + 39.5, cr + 4.55),
    'reset_access': box(9, 15, 10, cx - 14.7, cy + 34.5, cr + 4.2),
    'power_access': box(14, 12, 12, cx + 34, cy - 13, cr + 1.8),
    'sensor_plug_allowance': box(11, 14, 10, sx + 5, sy - 5.6, 4.1),
    'QT_return': box(22, 3.6, 3, px + 36.4, py + 8.89, pz + 1.9),
}
for port, k in keepouts.items():
    for name in names:
        v = k.intersect(parts[name]).val().Volume()
        if v > .01:
            report['access_intersections'].append({'port': port, 'part': name, 'mm3': v})
for i, sweep in enumerate(head_sweeps + head_entries):
    for name in names:
        v = sweep.intersect(parts[name]).val().Volume()
        if v > .01:
            report['wall_head_intersections'].append({'mount': i, 'part': name, 'mm3': v})
    for port, k in keepouts.items():
        v = sweep.intersect(k).val().Volume()
        if v > .01:
            report['wall_head_intersections'].append({'mount': i, 'port': port, 'mm3': v})
assert not report['access_intersections'], report['access_intersections']
assert not report['wall_head_intersections'], report['wall_head_intersections']

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--core', required=True)
    ap.add_argument('--adapter', required=True)
    ap.add_argument('--sensor', required=True)
    args = ap.parse_args()
    raw = trimesh.load(args.core)
    core = trimesh.util.concatenate([m for m in raw.split(only_watertight=False) if m.bounds[0, 2] > -30])
    core.apply_transform(np.array([[-1, 0, 0, cx], [0, 0, 1, cy], [0, 1, 0, cr + 5.3], [0, 0, 0, 1]]))
    core.export(ROOT / 'reference' / 'core_shell_reference.stl')
    lowered_core = core.copy()
    lowered_core.apply_translation([0, 0, -pad - .25])
    report['core_rear_support_contact_test'] = []
    for foot in core_supports:
        verts, faces = foot.val().tessellate(.03)
        mesh = trimesh.Trimesh([v.toTuple() for v in verts], faces)
        overlap = trimesh.boolean.intersection([lowered_core, mesh], engine='manifold')
        volume = 0 if not len(overlap.faces) else float(overlap.volume)
        report['core_rear_support_contact_test'].append(volume)
        assert volume > .05, ('Core support misses housing', volume)
    adapter = cq.importers.importStep(args.adapter).translate((px, py, pz))
    sensor = cq.importers.importStep(args.sensor).rotate((0, 0, 0), (1, 1, 1), 120).translate((sx, sy, sr))
    for name, obj in [('adapter', adapter), ('sensor', sensor)]:
        cq.exporters.export(obj, str(ROOT / 'reference' / (name + '_reference.stl')), tolerance=.04)
    report['hardware_intersections'] = {}
    for name in names:
        overlap = trimesh.boolean.intersection([core, trimesh.load(ROOT / 'assembly' / (name + '.stl'))], engine='manifold')
        r = {'core_mm3': 0 if len(overlap.faces) == 0 else float(overlap.volume),
             'sensor_mm3': parts[name].intersect(sensor).val().Volume(),
             'adapter_mm3': parts[name].intersect(adapter).val().Volume()}
        report['hardware_intersections'][name] = r
        assert all(v < .01 for v in r.values()), (name, r)
    for i, sweep in enumerate(head_sweeps + head_entries):
        verts, faces = sweep.val().tessellate(.03)
        mesh = trimesh.Trimesh([v.toTuple() for v in verts], faces)
        overlap = trimesh.boolean.intersection([core, mesh], engine='manifold')
        r = {'core_mm3': 0 if len(overlap.faces) == 0 else float(overlap.volume),
             'sensor_mm3': sweep.intersect(sensor).val().Volume(),
             'adapter_mm3': sweep.intersect(adapter).val().Volume()}
        report.setdefault('wall_head_hardware_intersections', {})[i] = r
        assert all(v < .01 for v in r.values()), ('Wall head sweep', i, r)
    # Shift hardware in both Y directions: a positive overlap proves the
    # stops actually catch its modeled shape, not just its bounding box.
    report['retention_test'] = {}
    for sign in (-1, 1):
        moved_core = core.copy()
        moved_core.apply_translation([0, sign * 1.5, 0])
        moved_sensor = sensor.translate((0, sign * 1.5, 0))
        cv = 0
        for stop in stops['core']:
            verts, faces = stop.val().tessellate(.03)
            mesh = trimesh.Trimesh([v.toTuple() for v in verts], faces)
            overlap = trimesh.boolean.intersection([moved_core, mesh], engine='manifold')
            cv += 0 if len(overlap.faces) == 0 else float(overlap.volume)
        sv = sum(stop.intersect(moved_sensor).val().Volume() for stop in stops['sensor'])
        report['retention_test'][str(sign)] = {'core_blocked_mm3': cv, 'sensor_blocked_mm3': sv}
        assert cv > 1 and sv > 1, ('Missing positive end stop', sign, cv, sv)
    (ROOT / 'validation.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2), flush=True)
