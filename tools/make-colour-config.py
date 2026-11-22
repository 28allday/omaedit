#!/usr/bin/env python3
"""Regenerate the immutable video extension with PyOpenColorIO and NumPy.

Oma Colour 1 is retained as the interchange contract. A released transform
change requires a new config ID, preserving this file for existing projects.
"""
from pathlib import Path
import numpy as np
import PyOpenColorIO as ocio

root = Path(__file__).resolve().parents[1]
config = ocio.Config.CreateFromFile(str(root / 'colour/oma-video-v1/oma-colour-1.ocio'))
config.setName('Oma Colour 1 - Video 1')
config.setVersion(2, 4)
log = ocio.ColorSpace(ocio.REFERENCE_SPACE_SCENE, name='Log2 Rec.709', bitDepth=ocio.BIT_DEPTH_F32, isData=False, encoding='log')
log.setTransform(ocio.LogTransform(base=2), ocio.COLORSPACE_DIR_FROM_REFERENCE)
config.addColorSpace(log)
config.setRole('compositing_log', 'Log2 Rec.709')
config.setRole('color_timing', 'rec709/rec709')
config.setDescription('Oma suite linear Rec.709 D65 interchange. Video component encodings; no tone mapping. PQ linear 1 = 100 cd/m2, preserving highlights above 1. HLG uses normalized inverse OETF, without display OOTF. MLT compositing/storage have integer boundaries.')
primaries = {
    'rec709': [(0.64, 0.33), (0.30, 0.60), (0.15, 0.06)],
    'srgb': [(0.64, 0.33), (0.30, 0.60), (0.15, 0.06)],
    'rec2020': [(0.708, 0.292), (0.170, 0.797), (0.131, 0.046)],
    'p3': [(0.68, 0.32), (0.265, 0.690), (0.150, 0.060)],
    'rec601': [(0.630, 0.340), (0.310, 0.595), (0.155, 0.070)],
}
def xyz(points):
    matrix = np.array([[x/y, 1, (1-x-y)/y] for x, y in points]).T
    white = np.array([0.3127/0.3290, 1, (1-0.3127-0.3290)/0.3290])
    return matrix @ np.diag(np.linalg.solve(matrix, white))

# Display-reference interchange for the design apps; no tone mapping.
display_xyz = ocio.ColorSpace(ocio.REFERENCE_SPACE_DISPLAY, name='CIE XYZ D65', bitDepth=ocio.BIT_DEPTH_F32, isData=False)
config.addColorSpace(display_xyz)
config.setRole('cie_xyz_d65_interchange', 'CIE XYZ D65')
to_xyz = np.eye(4)
to_xyz[:3, :3] = xyz(primaries['rec709'])
view = ocio.ViewTransform(ocio.REFERENCE_SPACE_SCENE, name='Un-tone-mapped')
view.setTransform(ocio.MatrixTransform(matrix=to_xyz.ravel()), ocio.VIEWTRANSFORM_DIR_FROM_REFERENCE)
config.addViewTransform(view)
config.setDefaultViewTransformName('Un-tone-mapped')

for gamut, points in primaries.items():
    matrix = np.eye(4)
    matrix[:3, :3] = np.linalg.solve(xyz(primaries['rec709']), xyz(points))
    for gamma in ['rec709', 'srgb', 'linear', '2.2', '2.4', '2.6', 'pq', 'hlg']:
        group = ocio.GroupTransform()
        if gamma in ['srgb', 'rec709']:
            # OCIO's continuous Rec.709 curve (break at ~0.08124) avoids a
            # discontinuity in the rounded broadcast coefficients.
            group.appendTransform(ocio.ExponentWithLinearTransform(
                gamma=[2.4 if gamma == 'srgb' else 1/0.45]*3+[1],
                offset=[0.055 if gamma == 'srgb' else 0.099]*3+[0]))
        elif gamma in ['2.2', '2.4', '2.6']:
            group.appendTransform(ocio.ExponentTransform(value=[float(gamma)]*3+[1], negativeStyle=ocio.NEGATIVE_MIRROR))
        elif gamma == 'pq':
            group.appendTransform(ocio.FixedFunctionTransform(style=ocio.FIXED_FUNCTION_LIN_TO_PQ, direction=ocio.TRANSFORM_DIR_INVERSE))
            # OCIO PQ uses linear 1 = 100 nits. Retain that convention;
            # 1000-nit highlights must remain 10 in the float working image.
        elif gamma == 'hlg':
            group.appendTransform(ocio.BuiltinTransform(style='CURVE - HLG-OETF-INVERSE'))
            # OCIO's builtin uses a peak linear value of 3; component HLG
            # here follows the normalized OETF (peak 1, half code = 1/12).
            group.appendTransform(ocio.MatrixTransform(matrix=np.diag([1/3, 1/3, 1/3, 1]).ravel()))
        group.appendTransform(ocio.MatrixTransform(matrix=matrix.ravel()))
        name = f'{gamut}/{gamma}'
        space = ocio.ColorSpace(ocio.REFERENCE_SPACE_SCENE, name=name, bitDepth=ocio.BIT_DEPTH_F32,
                               isData=False, encoding='scene-linear' if gamma == 'linear' else 'sdr-video')
        space.setTransform(group, ocio.COLORSPACE_DIR_TO_REFERENCE)
        config.addColorSpace(space)
        config.addDisplayView(name, 'Standard', name)
        config.addDisplayView(name, 'Linear diagnostic', f'{gamut}/linear')
config.setActiveDisplays('')
config.setActiveViews('')
config.validate()
(root / 'colour/oma-video-v1/config.ocio').write_text(config.serialize() + '\n')
