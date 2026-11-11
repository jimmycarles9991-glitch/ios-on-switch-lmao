"""The pipeline states of a JSON pipeline script.

A script is a list of MTLLibrary, MTLFunction (a specialization) and MTLRenderPipelineDescriptor /
MTLComputePipelineDescriptor objects, as PassKitUIFoundation's *-pipeline.json and WeatherUI's
NUNIPipelineState.mtlpipeline hold them. A .mtlp-json holding only {"functions": ...} declares no state.
"""

import json

from drivers.TegraGM20BMetal.corpus.tools.archives.model import (
    COMPUTE,
    RENDER,
    Attribute,
    Color,
    Constant,
    Function,
    Layout,
    Pipeline,
)

COLOR_KEYS = {
    'pixelFormat': 'format',
    'writeMask': 'write_mask',
    'blendingEnabled': 'blending',
    'sourceRGBBlendFactor': 'src_rgb',
    'destinationRGBBlendFactor': 'dst_rgb',
    'rgbBlendOperation': 'rgb_op',
    'sourceAlphaBlendFactor': 'src_alpha',
    'destinationAlphaBlendFactor': 'dst_alpha',
    'alphaBlendOperation': 'alpha_op',
}


class Script:
    def __init__(self, text: str, source: str, owner: str) -> None:
        objects = json.loads(text)
        self.objects = objects if isinstance(objects, list) else []
        self.specialized = {o['name']: o for o in self.objects if o.get('type') == 'MTLFunction'}
        self.source, self.owner = source, owner

    def pipelines(self) -> list[Pipeline]:
        out = []
        for o in self.objects:
            if o.get('type') == 'MTLRenderPipelineDescriptor':
                out.append(self._render(o))
            elif o.get('type') == 'MTLComputePipelineDescriptor':
                p = Pipeline(COMPUTE, self.source, self.owner, label=o.get('name') or '-')
                p.kernel = self._function(o.get('computeFunction'))
                out.append(p)
        return out

    def _function(self, ref: dict | None) -> Function | None:
        if not ref:
            return None
        spec = self.specialized.get(ref.get('name', ''))
        if not spec:
            return Function(ref['name'])
        # a constant naming neither its index nor its name is the function's constant at its position
        constants = [
            Constant(c.get('index', i) if 'name' not in c else None, c.get('name'), c['dataType'], _bytes(c))
            for i, c in enumerate(spec.get('constants', []))
        ]
        return Function(spec['baseFunction']['name'], constants=constants, specialized=True)

    def _render(self, o: dict) -> Pipeline:
        p = Pipeline(RENDER, self.source, self.owner, label=o.get('name') or '-')
        p.vertex, p.fragment = self._function(o.get('vertexFunction')), self._function(o.get('fragmentFunction'))
        p.samples = o.get('rasterSampleCount', o.get('sampleCount', 1))
        p.depth = o.get('depthAttachmentPixelFormat', 0)
        p.stencil = o.get('stencilAttachmentPixelFormat', 0)
        for c in o.get('colorAttachments', []):
            color = Color(c.get('index', 0), 0)
            for key, attr in COLOR_KEYS.items():
                if key in c:
                    setattr(color, attr, int(c[key]))
            p.colors.append(color)
        vertex = o.get('vertexDescriptor', {})
        for a in vertex.get('attributes', []):
            p.attributes.append(Attribute(a.get('index', 0), a['format'], a.get('offset', 0), a.get('bufferIndex', 0)))
        for lay in vertex.get('layouts', []):
            step = (lay.get('stepFunction', 1), lay.get('stepRate', 1))
            p.layouts[lay.get('index', 0)] = Layout(lay.get('stride', 0), *step)
        return p


def _bytes(constant: dict) -> bytes:
    value = constant['value']
    if isinstance(value, bool | int):
        return int(value).to_bytes(16, 'little', signed=value < 0)
    return b''
