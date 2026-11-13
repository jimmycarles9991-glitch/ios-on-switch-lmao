"""A blend's expected result, written the way a blend is read: `src.rgb * dst.rgb + src.rgb * (1 - dst.a)`.

Names: src and dst with .r .g .b .a or .rgb (a three-lane vector), c0..c7 with the same (an iterated blend constant).
Operators + - * / and unary -, numbers, and min, max, clamp(x, lo, hi), abs, saturate(x). Evaluated in float64 lane
by lane; a scalar meets a vector by repeating itself.
"""

import ast
import operator
from collections.abc import Callable

Value = float | tuple[float, float, float]
Colour = tuple[float, float, float, float]

BINARY: dict[type, Callable[[float, float], float]] = {
    ast.Add: operator.add,
    ast.Sub: operator.sub,
    ast.Mult: operator.mul,
    ast.Div: lambda a, b: a / b if b else (float('inf') if a > 0 else float('-inf') if a < 0 else float('nan')),
}
FUNCTIONS: dict[str, Callable[..., float]] = {
    'min': min,
    'max': max,
    'abs': abs,
    'clamp': lambda x, lo, hi: min(max(x, lo), hi),
    'saturate': lambda x: min(max(x, 0.0), 1.0),
}
CHANNELS = {'r': 0, 'g': 1, 'b': 2, 'a': 3}


class FormulaError(ValueError):
    pass


def _lanes(v: Value) -> tuple[float, float, float]:
    return v if isinstance(v, tuple) else (v, v, v)


def _map(f: Callable[..., float], *args: Value) -> Value:
    if not any(isinstance(a, tuple) for a in args):
        return f(*args)
    lanes = [_lanes(a) for a in args]
    return tuple(f(*(lane[i] for lane in lanes)) for i in range(3))


class Formula:
    def __init__(self, text: str) -> None:
        self.text = text
        try:
            self.tree = ast.parse(text, mode='eval').body
        except SyntaxError as e:
            raise FormulaError(f'{text!r}: {e.msg}') from e
        self._check(self.tree)

    def _check(self, node: ast.AST) -> None:
        allowed = (
            ast.BinOp,
            ast.UnaryOp,
            ast.Constant,
            ast.Attribute,
            ast.Name,
            ast.Call,
            ast.Load,
            ast.USub,
            ast.UAdd,
        )
        for n in ast.walk(node):
            if not isinstance(n, allowed + tuple(BINARY)):
                raise FormulaError(f'{self.text!r}: {type(n).__name__} is not allowed')
            if isinstance(n, ast.Call) and not (isinstance(n.func, ast.Name) and n.func.id in FUNCTIONS):
                raise FormulaError(f'{self.text!r}: only {sorted(FUNCTIONS)} may be called')

    def __call__(self, src: Colour, dst: Colour, constants: list[Colour] | None = None) -> Value:
        names = {'src': src, 'dst': dst} | {f'c{i}': c for i, c in enumerate(constants or [])}
        return self._eval(self.tree, names)

    def _eval(self, node: ast.AST, names: dict[str, Colour]) -> Value:
        if isinstance(node, ast.Constant) and isinstance(node.value, int | float):
            return float(node.value)
        if isinstance(node, ast.UnaryOp):
            v = self._eval(node.operand, names)
            return _map(operator.neg, v) if isinstance(node.op, ast.USub) else v
        if isinstance(node, ast.BinOp):
            return _map(BINARY[type(node.op)], self._eval(node.left, names), self._eval(node.right, names))
        if isinstance(node, ast.Call):
            return _map(FUNCTIONS[node.func.id], *(self._eval(a, names) for a in node.args))
        if isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name) and node.value.id in names:
            colour = names[node.value.id]
            if node.attr == 'rgb':
                return colour[0], colour[1], colour[2]
            if node.attr in CHANNELS:
                return colour[CHANNELS[node.attr]]
        raise FormulaError(f'{self.text!r}: cannot read {ast.unparse(node)}')


def blend(rgb: Formula, alpha: Formula, src: Colour, dst: Colour, constants: list[Colour] | None = None) -> Colour:
    r, g, b = _lanes(rgb(src, dst, constants))
    a = alpha(src, dst, constants)
    if isinstance(a, tuple):
        raise FormulaError(f'{alpha.text!r} is a vector; alpha is one lane')
    return r, g, b, a
