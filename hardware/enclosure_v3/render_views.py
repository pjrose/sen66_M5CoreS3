"""Render actual CAD, never a generative approximation of the printable design."""
import bpy
import json
from pathlib import Path
from mathutils import Vector

ROOT = Path(__file__).resolve().parent
P = json.loads((ROOT / 'parameters.json').read_text())
scene = bpy.context.scene
camera = scene.camera
tx, ty = P['width'] / 2000, P['height'] / 2000
scene.render.resolution_x, scene.render.resolution_y = 1500, 1200
if scene.render.engine == 'CYCLES':
    scene.cycles.samples = 32
    scene.cycles.use_denoising = True


def render(name, position, target, scale):
    camera.location = position
    camera.rotation_euler = (Vector(target) - camera.location).to_track_quat('-Z', 'Y').to_euler()
    bpy.context.view_layer.update()
    inverse = camera.matrix_world.inverted()
    projected = [inverse @ (ob.matrix_world @ Vector(corner))
        for ob in scene.objects if ob.type in ('MESH', 'CURVE') and not ob.hide_render
        for corner in ob.bound_box]
    aspect = scene.render.resolution_x / scene.render.resolution_y
    extent = max(max(abs(p.x) for p in projected), max(abs(p.y) for p in projected) * aspect)
    camera.data.ortho_scale = max(scale, 2 * extent * 1.10)
    scene.render.filepath = str(ROOT / 'renders' / (name + '.png'))
    bpy.ops.render.render(write_still=True)


render('assembly', (.15, -.065, .24), (tx, ty, .018), .15)
render('front', (tx, ty, .3), (tx, ty, .015), .133)
hidden = []
for ob in scene.objects:
    if any(s in ob.name for s in ('02_front_bezel', 'core_shell_reference', 'glass illustrative',
                                'Display illustrative', 'M3 countersunk', 'Screw drive')):
        ob.hide_render = True
        hidden.append(ob)
render('interior', (.14, -.08, .28), (tx, ty, .014), .15)
for ob in hidden:
    ob.hide_render = False
render('rear', (.16, .15, -.23), (tx, ty, .012), .15)
for ob in scene.objects:
    if ob.type in ('MESH', 'CURVE'):
        ob.hide_render = '01_vented_shell' not in ob.name
render('shell', (.14, -.08, .28), (tx, ty, .014), .15)
print('Rendered assembly, front, interior, rear and bare shell.')
