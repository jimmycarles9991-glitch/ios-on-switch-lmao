import base64
import json
import re
import sys
from pathlib import Path

if len(sys.argv) == 1:
    d = {
        'stages': [{'stage': s, 'entry': 'main', 'subgroup': 0, 'flags': 0, 'map': [], 'data': ''} for s in (1, 16)],
        'sets': [],
        'push': [],
        'color_formats': [70],
        'depth_format': 0,
        'stencil_format': 0,
        'storage': False,
        'storage_mask': 0,
        'samples': 1,
        'sample_shading': False,
    }
    corpus = 'samples=1 topo=0 c0=70:15:0:1:0:0:1:0:0 a0=31:0:0:16:1:1'
elif len(sys.argv) == 3:
    d = json.loads(Path(sys.argv[1]).read_bytes())
    corpus = Path(sys.argv[2]).read_text().split(' | ')[0]
else:
    raise SystemExit('usage: layout.py [pipeline.json corpus.log]')
if [stage['stage'] for stage in d['stages']] != [1, 16] or len(d['sets']) > 32:
    raise ValueError('expected one vertex/fragment pipeline')
if d['depth_format'] != d['stencil_format'] or 'topo=0' not in corpus:
    raise ValueError('unsupported depth/stencil or topology')
out = []


def array(typ: str, name: str, items: list[str]) -> None:
    out.append('static const ' + typ + ' ' + name + '[] = {' + (','.join(items) or '{0}') + '};')


def num(x: int) -> str:
    return str(int(x))


def row(*items: object) -> str:
    return '{' + ','.join(map(str, items)) + '}'


for i, st in enumerate(d['stages']):
    if st['entry'] != 'main' or st['subgroup'] or st['flags']:
        raise ValueError(st)
    array('VkSpecializationMapEntry', 'entries' + str(i), [row(*e) for e in st['map']])
    array('unsigned char', 'data' + str(i), [num(x) for x in base64.b64decode(st['data'])] or ['0'])
out.append(
    'static const VkSpecializationInfo specs[] = {'
    + ','.join(
        row(len(s['map']), 'entries' + str(i), len(base64.b64decode(s['data'])), 'data' + str(i))
        for i, s in enumerate(d['stages'])
    )
    + '};'
)
sampler_states = []
out.append('static VkSampler immutable_handles[256];')
for i, s in enumerate(d['sets']):
    if s['flags']:
        raise ValueError(s)
    rows = []
    for b, typ, n, flags, immutable in s['bindings']:
        if flags:
            raise ValueError('descriptor binding flags')
        handle = 'NULL'
        if immutable:
            values = s['samplers'][str(b)]
            if n != 1 or values[-1] != 0 or values[-3] >= 1000000000 or len(sampler_states) >= 256:
                raise ValueError('unsupported sampler')
            handle = '&immutable_handles[' + str(len(sampler_states)) + ']'
            sampler_states.append(
                row(
                    'VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO',
                    'NULL',
                    0,
                    *[str(int(v)) if isinstance(v, bool) else v for v in values[:-1]],
                )
            )
        rows.append(row(b, typ, n, 17, handle))
    array('VkDescriptorSetLayoutBinding', 'bindings' + str(i), rows)
array('VkSamplerCreateInfo', 'sampler_states', sampler_states)
out.append('#define NUM_SAMPLERS ' + str(len(sampler_states)))
array(
    'VkDescriptorSetLayoutCreateInfo',
    'layouts',
    [
        row('VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO', 'NULL', 0, len(s['bindings']), 'bindings' + str(i))
        for i, s in enumerate(d['sets'])
    ],
)
array('VkPushConstantRange', 'ranges', [row(*v) for v in d['push']])
formats = {
    0: 'VK_FORMAT_UNDEFINED',
    70: 'VK_FORMAT_R8G8B8A8_UNORM',
    542: 'VK_FORMAT_R16G16B16A16_SFLOAT',
    80: 'VK_FORMAT_B8G8R8A8_UNORM',
    81: 'VK_FORMAT_B8G8R8A8_SRGB',
    115: 'VK_FORMAT_R16G16B16A16_SFLOAT',
    55: 'VK_FORMAT_R32_SFLOAT',
    105: 'VK_FORMAT_R32G32_SFLOAT',
    260: 'VK_FORMAT_D32_SFLOAT_S8_UINT',
}
vf = {
    12: 'VK_FORMAT_R8G8B8A8_SNORM',
    19: 'VK_FORMAT_R16G16_UNORM',
    22: 'VK_FORMAT_R16G16_SNORM',
    27: 'VK_FORMAT_R16G16B16A16_SFLOAT',
    28: 'VK_FORMAT_R32_SFLOAT',
    29: 'VK_FORMAT_R32G32_SFLOAT',
    31: 'VK_FORMAT_R32G32B32A32_SFLOAT',
}
attrs = []
bind = {}
for i, t, offset, b, stride, step, rate in re.findall(r'a(\d+)=(\d+):(\d+):(\d+):(\d+):(\d+):(\d+)', corpus):
    if int(step) != 1 or int(rate) != 1:
        raise ValueError('vertex rate')
    bind[b] = stride
    attrs.append(row(i, b, vf[int(t)], offset))
array('VkVertexInputBindingDescription', 'vertex_bindings', [row(b, v, 0) for b, v in bind.items()])
array('VkVertexInputAttributeDescription', 'vertex_attrs', attrs)
colors = [f for f in d['color_formats'] if f]
array('VkFormat', 'color_formats', [formats[f] for f in colors])
blends = re.findall(r'c(\d+)=(\d+):(\d+):(\d+):([^ ]+)', corpus)
if (
    d['color_formats'][: len(colors)] != colors
    or [int(b[0]) for b in blends] != list(range(len(colors)))
    or [int(b[1]) for b in blends] != colors
):
    raise ValueError('sparse or mismatched attachment list')
for _, _, _, enable, _ in blends:
    if int(enable):
        raise ValueError('unimplemented blend ' + corpus)
array(
    'VkPipelineColorBlendAttachmentState',
    'color_blends',
    [
        '{.colorWriteMask='
        + num(((int(mask) & 8) >> 3) | ((int(mask) & 4) >> 1) | ((int(mask) & 2) << 1) | ((int(mask) & 1) << 3))
        + '}'
        for _, _, mask, _, _ in blends
    ],
)
for name, val in {
    'NUM_SETS': len(d['sets']),
    'NUM_PUSH': len(d['push']),
    'NUM_ATTRS': len(attrs),
    'NUM_BINDINGS': len(bind),
    'NUM_COLORS': len(colors),
    'SAMPLES': d['samples'],
    'SAMPLE_SHADING': int(d['sample_shading']),
    'DEPTH_FORMAT': formats[d['depth_format']],
    'INTERLOCK': int(d['storage']),
    'STORAGE_MASK': d['storage_mask'],
}.items():
    out.append('#define ' + name + ' ' + str(val))
print('\n'.join(out))
