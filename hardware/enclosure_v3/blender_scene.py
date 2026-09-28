"""Load dimensioned CAD into a separate Blender scene, in metres.

Hardware shells come from reference CAD. Display, cables and screw heads are
illustrative only and are not included in printable meshes.
"""
import bpy
import math
import json
from pathlib import Path
from mathutils import Vector

ROOT = Path(__file__).resolve().parent
PREFIX = 'AQV3_'
P = json.loads((ROOT / 'parameters.json').read_text())
face = P['sensor_rear_z'] + P['sensor_depth']
joint = face + P['compressed_pad']
front = joint + P['face_thickness']
cr = face - P['core_depth']
cx, cy = P['core_center']
sx, sy = P['sensor_center']
px, py, pz = P['pcb_origin']
target = (P['width'] / 2000, P['height'] / 2000, .015)


def enum_set(obj, prop, value):
    allowed = [i.identifier for i in obj.bl_rna.properties[prop].enum_items]
    if value not in allowed:
        raise ValueError((prop, value, allowed))
    setattr(obj, prop, value)


old = bpy.data.scenes.get(PREFIX + 'Assembly')
if old:
    for ob in list(old.objects):
        bpy.data.objects.remove(ob, do_unlink=True)
    bpy.data.scenes.remove(old)
scene = bpy.data.scenes.new(PREFIX + 'Assembly')
bpy.context.window.scene = scene
enum_set(scene.unit_settings, 'system', 'METRIC')
scene.unit_settings.scale_length = 1
# The dynamic length-unit enum was inspected on the connected Blender 5.2.
scene.unit_settings.length_unit = 'MILLIMETERS'


def material(name, color, metal=0, rough=.45):
    m = bpy.data.materials.new(PREFIX + name)
    m.diffuse_color = (*color, 1)
    node = next(n for n in m.node_tree.nodes if n.type == 'BSDF_PRINCIPLED')
    node.inputs['Base Color'].default_value = (*color, 1)
    node.inputs['Roughness'].default_value = rough
    node.inputs['Metallic'].default_value = metal
    return m


pla = material('PLA muted teal', (.10, .36, .34))
cap = material('PLA faceplate', (.53, .57, .56))
black = material('SEN housing', (.023, .027, .029))
gray = material('Core housing', (.30, .32, .35))
pcb = material('Adapter PCB', (.035, .09, .06))
glass = material('Display reference', (.009, .018, .024), rough=.23)
metal = material('Screw heads', (.5, .53, .56), .8, .3)
brass = material('Heat set brass', (.63, .41, .12), .75, .32)
amber = material('QT routing study', (.95, .49, .05))
blue = material('SEN cable routing study', (.12, .42, .84))


def load_stl(path, mat):
    bpy.ops.wm.stl_import(filepath=str(path), global_scale=.001)
    ob = bpy.context.object
    ob.name = PREFIX + path.stem
    ob.data.materials.clear()
    ob.data.materials.append(mat)
    return ob


for f in sorted((ROOT / 'assembly').glob('*.stl')):
    if not f.stem.startswith(('01', '02')):
        continue
    ob = load_stl(f, pla if f.stem.startswith('01') else cap)
    if f.stem.startswith('02'):
        ob.data.materials.append(black)
        for polygon in ob.data.polygons:
            if polygon.center.z * ob.scale.z > front * .001 + .00001:
                polygon.material_index = 1
load_stl(ROOT / 'reference' / 'sensor_reference.stl', black)
load_stl(ROOT / 'reference' / 'core_shell_reference.stl', gray)
ob = load_stl(ROOT / 'reference' / 'adapter_reference.stl', pcb)
ob.data.materials.append(gray)
for polygon in ob.data.polygons:
    if polygon.center.z * ob.scale.z > (pz + 1.6) * .001:
        polygon.material_index = 1


def cube(name, loc, size, mat):
    bpy.ops.mesh.primitive_cube_add(size=1, location=Vector(loc) * .001)
    ob = bpy.context.object
    ob.name = PREFIX + name
    ob.dimensions = Vector(size) * .001
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    ob.data.materials.append(mat)
    return ob


cube('Front glass illustrative only', (cx, cy, face - .05), (51, 51, .2), glass)
cube('Display illustrative only', (cx, cy - 3, face + .11), (40.6, 30.5, .12), gray)
inset = P['corner_screw_inset']
for x in (inset, P['width'] - inset):
    for y in (inset, P['height'] - inset):
        bpy.ops.mesh.primitive_cone_add(vertices=32, radius1=.0015, radius2=.0028,
            depth=.0013, location=(x * .001, y * .001, (front - .7) * .001))
        ob = bpy.context.object
        ob.name = PREFIX + 'M3 countersunk screw'
        ob.data.materials.append(metal)
        cube('Screw drive reference', (x, y, front - .03), (2.6, .6, .1), black)
for x0 in (2.54, 22.86):
    for y0 in (2.54, 15.24):
        x, y = px + x0, py + y0
        bpy.ops.mesh.primitive_cylinder_add(vertices=32, radius=.00215, depth=.0015,
            location=(x * .001, y * .001, (pz + 1.57 + .75) * .001))
        ob = bpy.context.object
        ob.name = PREFIX + 'Adapter M2.5 fastener reference'
        ob.data.materials.append(metal)
        cube('Adapter drive reference', (x, y, pz + 3.1), (2, .5, .08), black)


def route(name, points, mat, r=.9):
    data = bpy.data.curves.new(PREFIX + name, 'CURVE')
    enum_set(data, 'dimensions', '3D')
    data.bevel_depth = r * .001
    data.bevel_resolution = 3
    spline = data.splines.new('POLY')
    spline.points.add(len(points) - 1)
    for p, co in zip(spline.points, points):
        p.co = (*[c * .001 for c in co], 1)
    ob = bpy.data.objects.new(PREFIX + name, data)
    scene.collection.objects.link(ob)
    data.materials.append(mat)
    return ob


radius = max(8.5, P['qt_bend_radius'])
z_start, z_end = cr + 7.4, pz + 3.4
z_entry = cr + 1.2
qt = [(cx + 27, cy + 14, z_start), (cx + 37, cy + 14, z_start)]
for i in range(1, 25):
    t = i * math.pi / 24
    qt.append((cx + 37 + radius * math.sin(t), cy + 14 - radius + radius * math.cos(t),
               z_start + (z_entry - z_start) * i / 24))
qt += [(103.3, cy - 3, z_entry), (103.3, cy - 3, z_end), (px + 25.4, py + 8.89, z_end)]
route('QT approximate cable', qt, amber)
route('SEN approximate cable', [(sx + 5, sy - 5.6, 15), (sx + 5, sy - 5.6, 8),
    (35, sy - 5.6, 8), (44, sy - 3, 8), (55, sy - 3, 8), (56, cy + 14, 9),
    (66, cy + 14, 9), (74, cy + 11, 9), (74, py + 17.09, 9)], blue)

world = bpy.data.worlds.new(PREFIX + 'World')
scene.world = world
world.use_nodes = True
bg = next(n for n in world.node_tree.nodes if n.type == 'BACKGROUND')
bg.inputs['Color'].default_value = (.55, .6, .66, 1)
bg.inputs['Strength'].default_value = .35


def aim(ob, point):
    ob.rotation_euler = (Vector(point) - ob.location).to_track_quat('-Z', 'Y').to_euler()


for name, loc, power, size in [('Key', (-.04, .14, .23), .35, .18),
                              ('Fill', (.2, .04, .13), .15, .14),
                              ('Rim', (.04, -.1, .08), .12, .10)]:
    data = bpy.data.lights.new(PREFIX + name, 'AREA')
    data.energy, data.size = power, size
    ob = bpy.data.objects.new(PREFIX + name, data)
    scene.collection.objects.link(ob)
    ob.location = loc
    aim(ob, target)
data = bpy.data.cameras.new(PREFIX + 'Camera')
camera = bpy.data.objects.new(PREFIX + 'Camera', data)
scene.collection.objects.link(camera)
scene.camera = camera
enum_set(data, 'type', 'ORTHO')
data.ortho_scale = .158
camera.location = (.15, -.065, .24)
aim(camera, target)
scene.render.resolution_x, scene.render.resolution_y = 1500, 1200
scene.render.resolution_percentage = 100
enum_set(scene.render.image_settings, 'file_format', 'PNG')
scene.render.film_transparent = False
try:
    scene.render.engine = 'CYCLES'
except TypeError:
    pass
if scene.render.engine == 'CYCLES':
    scene.cycles.samples = 32
for screen in bpy.data.screens:
    for area in screen.areas:
        if area.type == 'VIEW_3D':
            area.spaces.active.region_3d.view_distance = .28
            area.spaces.active.region_3d.view_location = target
            area.spaces.active.region_3d.view_rotation = camera.rotation_euler.to_quaternion()
            enum_set(area.spaces.active.region_3d, 'view_perspective', 'ORTHO')
            enum_set(area.spaces.active.shading, 'color_type', 'MATERIAL')
            area.spaces.active.overlay.show_overlays = False
scene.render.filepath = str(ROOT / 'renders' / 'assembly.png')
bpy.ops.wm.save_as_mainfile(filepath=str(ROOT / 'enclosure.blend'))
print('Loaded v3 CAD in its own review scene; older scenes preserved.')
