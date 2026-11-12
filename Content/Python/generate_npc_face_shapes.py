"""Blender 后台生成 NPC 眼部形态键，只输出独立副本，不修改源模型或骨架

运行 blender --background --python Content/Python/generate_npc_face_shapes.py
选择依据来自源模型连通区域检查，数量和范围不符时停止，避免错改脸部
"""
import bpy
import math
import json
from pathlib import Path
from mathutils import Vector, Matrix

ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'Design/Art/NPC/Residents/Appearance'
OUT.mkdir(parents=True,exist_ok=True)
report=[]

def islands(mesh):
    neighbors=[[] for _ in mesh.vertices]
    for edge in mesh.edges:
        a,b=edge.vertices;neighbors[a].append(b);neighbors[b].append(a)
    remaining=set(range(len(neighbors)))
    while remaining:
        pending=[remaining.pop()];ids=[]
        while pending:
            index=pending.pop();ids.append(index)
            for neighbor in neighbors[index]:
                if neighbor in remaining:remaining.remove(neighbor);pending.append(neighbor)
        yield ids

for source,name in [('中年男','SK_NpcManFace'),('中年女','SK_NpcWomanFace')]:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=str(ROOT/'Design/Art/NPC'/f'{source}.fbx'))
    mesh=next(o for o in bpy.data.objects if o.type=='MESH')
    rig=next(o for o in bpy.data.objects if o.type=='ARMATURE')
    world=mesh.matrix_world.copy();inverse=world.inverted()
    eyes=[];brows=[]
    for ids in islands(mesh.data):
        points=[world@mesh.data.vertices[i].co for i in ids]
        low=Vector(tuple(min(p[a] for p in points) for a in range(3)))
        high=Vector(tuple(max(p[a] for p in points) for a in range(3)))
        if low.x>.15 and .69<low.z<.72 and .77<high.z<.80 and len(ids)<120:
            eyes.append((ids,(low+high)*.5))
        if low.x>.14 and .78<low.z<.81 and high.z<.85 and len(ids)<100:
            brows.append((ids,(low+high)*.5))
    assert len(eyes)==2,(source,'expected two independent eye regions',len(eyes))
    # 源贴图把眼睛和眉毛的暗色烘焙到了脸上，移动几何体后会留下重影
    # 单独记录脸部遮罩，材质用邻近皮肤色覆盖烘焙痕迹，眼睛本身不受影响
    eye_ids={i for ids,_ in eyes for i in ids}
    brow_ids={i for ids,_ in brows for i in ids}
    mask=mesh.data.color_attributes.new(name='FaceClean',type='FLOAT_COLOR',domain='CORNER')
    regions=[(center,.035,.060) for _,center in eyes]
    if len(brows)==2:regions += [(center,.052,.034) for _,center in brows]
    for loop in mesh.data.loops:
        index=loop.vertex_index;point=world@mesh.data.vertices[index].co
        weight=0
        if index not in eye_ids|brow_ids and point.x>.12:
            for center,ry,rz in regions:
                radius=math.sqrt(((point.y-center.y)/ry)**2+((point.z-center.z)/rz)**2)
                weight=max(weight,max(0,min(1,(1.5-radius)/.45)))
        if source=='中年女' and point.z>.778:
            weight=0
        mask.data[loop.index].color=(weight,weight,weight,1)
    material=mesh.data.materials[0];tree=material.node_tree
    bsdf=next(n for n in tree.nodes if n.type=='BSDF_PRINCIPLED')
    old=bsdf.inputs['Base Color'].links[0].from_socket
    # 直接取两眼之间的已有贴图颜色，不编辑原纹理文件
    image=next(n.image for n in tree.nodes if n.type=='TEX_IMAGE' and n.image)
    reference=min((loop for loop in mesh.data.loops if loop.vertex_index not in eye_ids|brow_ids),key=lambda loop:((world@mesh.data.vertices[loop.vertex_index].co)-Vector((.18,0,.75))).length)
    uv=mesh.data.uv_layers.active.data[reference.index].uv
    x=int(uv.x*(image.size[0]-1));y=int(uv.y*(image.size[1]-1));pixel=(y*image.size[0]+x)*4
    skin=tuple(image.pixels[pixel:pixel+4])
    color=tree.nodes.new('ShaderNodeVertexColor');color.layer_name='FaceClean'
    mix=tree.nodes.new('ShaderNodeMixRGB');mix.blend_type='MIX';mix.inputs[2].default_value=skin
    tree.links.new(old,mix.inputs[1]);tree.links.new(color.outputs['Color'],mix.inputs[0]);tree.links.new(mix.outputs[0],bsdf.inputs['Base Color'])
    mesh.shape_key_add(name='Basis')
    base=[v.co.copy() for v in mesh.data.vertices]
    def empty_shape(name):
        shape=mesh.shape_key_add(name=name)
        # 每个表情从原始脸型开始，随后只修改对应的眼睛或眉毛
        for index,point in enumerate(base):shape.data[index].co=point
        return shape
    def make(name,transform):
        shape=empty_shape(name)
        for ids,center in eyes:
            for index in ids:
                point=world@base[index]
                shape.data[index].co=inverse@transform(point,center)
        return shape
    make('EyesBlink',lambda p,c: Vector((p.x+.001,p.y,c.z+(p.z-c.z)*.025)))
    make('EyesLookLeft',lambda p,c:p+Vector((.001,-.010,0)))
    make('EyesLookRight',lambda p,c:p+Vector((.001,.010,0)))
    make('EyesLookUp',lambda p,c:p+Vector((.001,0,.010)))
    make('EyesLookDown',lambda p,c:p+Vector((.001,0,-.010)))
    def angry(p,c):
        offset=p-c;offset.z*=.35
        # 两眼向内压低，旋转绕面部朝向轴进行
        offset=Matrix.Rotation(math.radians(28 if c.y>0 else -28),3,'X')@offset
        return c+offset+Vector((.001,0,0))
    make('EyesAngry',angry)
    shape=make('FaceAngry',angry)
    brow_shape=empty_shape('BrowsAngry')
    # 男角色眉毛独立，可一并旋转；女角色一侧连着头部，保留原眉毛以免拉动头发
    if len(brows)==2:
        for ids,center in brows:
            rotation=Matrix.Rotation(math.radians(32 if center.y>0 else -32),3,'X')
            for index in ids:
                point=inverse@(center+rotation@((world@base[index])-center)+Vector((.001,0,-.010)))
                shape.data[index].co=point
                brow_shape.data[index].co=point
    make('FaceSurprised',lambda p,c:c+Vector(((p-c).x+.001,(p-c).y*1.35,(p-c).z*1.35)))
    make('FaceListening',lambda p,c:c+Vector(((p-c).x+.001,(p-c).y,(p-c).z*(1.12 if c.y>0 else .8)))+Vector((0,0,.008 if c.y>0 else 0)))
    make('FaceThinking',lambda p,c:c+Vector(((p-c).x+.001,(p-c).y,(p-c).z*(.35 if c.y>0 else .8))))
    bpy.ops.object.select_all(action='DESELECT');mesh.select_set(True);rig.select_set(True)
    bpy.context.view_layer.objects.active=rig
    bpy.ops.export_scene.fbx(filepath=str(OUT/f'{name}.fbx'),use_selection=True,object_types={'ARMATURE','MESH'},
        add_leaf_bones=False,bake_anim=False,mesh_smooth_type='FACE',use_mesh_modifiers=False,path_mode='AUTO')
    # 留下可重复渲染的预览文件，仅保存在不提交的 Saved 目录
    bpy.ops.wm.save_as_mainfile(filepath=str(ROOT/f'Saved/AIResearch/{name}.blend'))
    report.append({'mesh':name,'skin_color':skin,'eyes':[len(ids) for ids,_ in eyes],'separate_brows':len(brows),
                   'morphs':[key.name for key in mesh.data.shape_keys.key_blocks],'vertex_count':len(mesh.data.vertices)})
(ROOT/'Saved/AIResearch/face-shapes.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
(OUT/'face-shapes.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
