#!/usr/bin/env python3
"""Generate oma-video-v2: everything in oma-video-v1, unchanged, plus camera
formats, ACES spaces and tone-mapped display views. v1 stays as it is, so grades
pinned to it keep their meaning; a released transform needs a new config ID.
"""
from pathlib import Path
import numpy as np
import PyOpenColorIO as ocio

root = Path(__file__).resolve().parents[1]
config = ocio.Config.CreateFromFile(str(root / 'colour/oma-video-v1/config.ocio'))
config.setName('Oma Colour 1 - Video 2')
config.setVersion(2, 5)
config.setDescription(config.getDescription() + ' Video 2 adds camera formats, ACES spaces and tone-mapped views; every Video 1 transform is unchanged.')

WHITE = (0.3127, 0.3290)
def to_xyz(points, white=WHITE):
    matrix = np.array([[x/y, 1, (1-x-y)/y] for x, y in points]).T
    w = np.array([white[0]/white[1], 1, (1-white[0]-white[1])/white[1]])
    return matrix @ np.diag(np.linalg.solve(matrix, w))
def m44(m3):
    m = np.eye(4); m[:3, :3] = m3
    return ocio.MatrixTransform(matrix=m.ravel())
REC709 = [(0.64, 0.33), (0.30, 0.60), (0.15, 0.06)]
primaries = {
    'rec709': REC709, 'srgb': REC709,
    'rec2020': [(0.708, 0.292), (0.170, 0.797), (0.131, 0.046)],
    'p3': [(0.68, 0.32), (0.265, 0.690), (0.150, 0.060)],
    'rec601': [(0.630, 0.340), (0.310, 0.595), (0.155, 0.070)],
}
def scene(name, *steps, encoding='log', family='Camera', description=''):
    group = ocio.GroupTransform()
    for step in steps:
        group.appendTransform(step)
    space = ocio.ColorSpace(ocio.REFERENCE_SPACE_SCENE, name=name, family=family, bitDepth=ocio.BIT_DEPTH_F32,
                            isData=False, encoding=encoding, description=description)
    space.setTransform(group, ocio.COLORSPACE_DIR_TO_REFERENCE)
    config.addColorSpace(space)

# Through ACES2065-1: OCIO's own camera transforms, and the bridge the config already has.
def aces_to_working():
    return ocio.ColorSpaceTransform(src='ACES2065-1', dst='Linear Rec.709')
for name, style in {
    'arri/logc3': 'ARRI_ALEXA-LOGC-EI800-AWG_to_ACES2065-1',
    'arri/logc4': 'ARRI_LOGC4_to_ACES2065-1',
    'sony/slog3-sgamut3': 'SONY_SLOG3-SGAMUT3_to_ACES2065-1',
    'sony/slog3-sgamut3cine': 'SONY_SLOG3-SGAMUT3.CINE_to_ACES2065-1',
    'sony/slog3-venice-sgamut3': 'SONY_SLOG3-SGAMUT3-VENICE_to_ACES2065-1',
    'sony/slog3-venice-sgamut3cine': 'SONY_SLOG3-SGAMUT3.CINE-VENICE_to_ACES2065-1',
    'red/log3g10': 'RED_LOG3G10-RWG_to_ACES2065-1',
    'red/logfilm': 'RED_REDLOGFILM-RWG_to_ACES2065-1',
    'canon/log2': 'CANON_CLOG2-CGAMUT_to_ACES2065-1',
    'canon/log3': 'CANON_CLOG3-CGAMUT_to_ACES2065-1',
    'panasonic/vlog': 'PANASONIC_VLOG-VGAMUT_to_ACES2065-1',
    'apple/log': 'APPLE_LOG_to_ACES2065-1',
}.items():
    scene(name, ocio.BuiltinTransform(style=style), aces_to_working())

# The rest from their makers' published curves and primaries: a log and a
# straight segment, then the camera's gamut into linear Rec.709.
def log_curve(base, log_slope, log_offset, lin_slope, lin_offset, lin_break, linear_slope=None):
    curve = ocio.LogCameraTransform(linSideBreak=[lin_break]*3, base=base, logSideSlope=[log_slope]*3, logSideOffset=[log_offset]*3,
                                    linSideSlope=[lin_slope]*3, linSideOffset=[lin_offset]*3, direction=ocio.TRANSFORM_DIR_INVERSE)
    if linear_slope is not None:
        curve.setLinearSlopeValue([linear_slope]*3)
    return curve
def gamut(points):
    return m44(np.linalg.solve(to_xyz(REC709), to_xyz(points)))
F_GAMUT = primaries['rec2020']
D_GAMUT = [(0.71, 0.31), (0.21, 0.88), (0.09, -0.08)]
BMD_WIDE = [(0.7177215, 0.3171181), (0.228041, 0.861569), (0.1005841, -0.0820452)]
CINEMA_GAMUT = [(0.74, 0.27), (0.17, 1.14), (0.08, -0.10)]
scene('fuji/flog', log_curve(10, 0.344676, 0.790453, 0.555556, 0.009468, 0.00089, 8.735631), gamut(F_GAMUT))
scene('fuji/flog2', log_curve(10, 0.245281, 0.384316, 5.555556, 0.064829, 0.000889, 8.799461), gamut(F_GAMUT))
scene('dji/dlog', log_curve(10, 0.256663, 0.584555, 0.9892, 0.0108, 0.0078, 6.025), gamut(D_GAMUT))
scene('blackmagic/gen5', log_curve(np.e, 0.08692876065491224, 0.5300133392291939, 1, 0.005494072432257808, 0.005, 8.283605932402494), gamut(BMD_WIDE))
scene('canon/log', log_curve(10, 0.529136, 0.0730597, 10.1596 / 0.9, 1, 0), gamut(CINEMA_GAMUT))

# ACES working and grading spaces.
scene('aces/cg', ocio.BuiltinTransform(style='ACEScg_to_ACES2065-1'), aces_to_working(), encoding='scene-linear', family='ACES')
scene('aces/cc', ocio.BuiltinTransform(style='ACEScc_to_ACES2065-1'), aces_to_working(), family='ACES')
scene('aces/cct', ocio.BuiltinTransform(style='ACEScct_to_ACES2065-1'), aces_to_working(), family='ACES')
scene('aces/2065-1', aces_to_working(), encoding='scene-linear', family='ACES')

# Tone mapping: a look from scene light to the display's light, as view
# transforms from the working space, each with its ACES output for that target.
ACES_OUT = 'ACES-OUTPUT - ACES2065-1_to_CIE-XYZ-D65 - '
TONE = {
    'ACES 2.0': {'sdr': 'SDR-100nit-REC709_2.0', 'sdr-wide': 'SDR-100nit-P3-D65_2.0',
                 'hdr': 'HDR-1000nit-P3-D65_2.0', 'hdr-wide': 'HDR-1000nit-REC2020_2.0'},
    'ACES 1.3': {'sdr': 'SDR-VIDEO_1.0', 'sdr-wide': 'SDR-VIDEO-P3lim_1.1',
                 'hdr': 'HDR-VIDEO-1000nit-15nit-P3lim_1.1', 'hdr-wide': 'HDR-VIDEO-1000nit-15nit-REC2020lim_1.1'},
}
def view_transform(tone, target):
    name = f'{tone} {target}'
    if name not in config.getViewTransformNames():
        group = ocio.GroupTransform()
        group.appendTransform(ocio.ColorSpaceTransform(src='Linear Rec.709', dst='ACES2065-1'))
        group.appendTransform(ocio.BuiltinTransform(style=ACES_OUT + TONE[tone][target]))
        view = ocio.ViewTransform(ocio.REFERENCE_SPACE_SCENE, name=name)
        view.setTransform(group, ocio.VIEWTRANSFORM_DIR_FROM_REFERENCE)
        config.addViewTransform(view)
    return name
def encoding(gamma):
    """Display light (1 = 100 cd/m2) to what the display wants: by its EOTF,
    as a tone map has already decided the picture; Rec.709 is BT.1886's 2.4."""
    if gamma == 'srgb':
        return ocio.ExponentWithLinearTransform(gamma=[2.4]*3+[1], offset=[0.055]*3+[0], direction=ocio.TRANSFORM_DIR_INVERSE)
    if gamma == 'pq':
        return ocio.FixedFunctionTransform(style=ocio.FIXED_FUNCTION_LIN_TO_PQ)
    power = {'rec709': 2.4, '2.2': 2.2, '2.4': 2.4, '2.6': 2.6}[gamma]
    return ocio.ExponentTransform(value=[power]*3+[1], negativeStyle=ocio.NEGATIVE_MIRROR, direction=ocio.TRANSFORM_DIR_INVERSE)
def display_space(gamut_name, gamma):
    name = f'display {gamut_name}/{gamma}'
    group = ocio.GroupTransform()
    if gamma == 'hlg':
        group.appendTransform(ocio.BuiltinTransform(style='DISPLAY - CIE-XYZ-D65_to_REC.2100-HLG-1000nit'))
    else:
        group.appendTransform(m44(np.linalg.inv(to_xyz(primaries[gamut_name]))))
        group.appendTransform(encoding(gamma))
    space = ocio.ColorSpace(ocio.REFERENCE_SPACE_DISPLAY, name=name, family='Display', bitDepth=ocio.BIT_DEPTH_F32,
                            isData=False, encoding='hdr-video' if gamma in ('pq', 'hlg') else 'sdr-video')
    space.setTransform(group, ocio.COLORSPACE_DIR_FROM_REFERENCE)
    config.addColorSpace(space)
    return name
for gamut_name in primaries:
    for gamma in ['rec709', 'srgb', '2.2', '2.4', '2.6', 'pq', 'hlg']:
        hdr = gamma in ('pq', 'hlg')
        if gamma == 'hlg' and gamut_name != 'rec2020':
            continue  # HLG's display transform is Rec.2020's alone
        wide = gamut_name in ('rec2020',) if hdr else gamut_name in ('rec2020', 'p3')
        target = ('hdr' if hdr else 'sdr') + ('-wide' if wide else '')
        space = display_space(gamut_name, gamma)
        for tone in TONE:
            config.addDisplayView(f'{gamut_name}/{gamma}', tone, view_transform(tone, target), space, '', '', '')

config.setActiveDisplays('')
config.setActiveViews('')
config.validate()
out = root / 'colour/oma-video-v2/config.ocio'
out.parent.mkdir(exist_ok=True)
out.write_text(config.serialize() + '\n')
