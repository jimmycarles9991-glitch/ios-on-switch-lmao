// SPDX-License-Identifier: MIT
//! A tessellation state's modes for the raw path's direct compile (NativeMetalDirect.h NmbDirectTess): its
//! SET_TESSELLATION_PARAMETERS, and its evaluation stage's module given the control stage's tessellation modes.
//! m2v declares the modes in the control stage alone, as Vulkan allows, and a pipeline's link merges them into both stages
//! (vk_pipeline_tess_info_merge); NVK's direct compile builds each stage alone, and NAK lowers the evaluation stage's
//! TessCoord by the domain that stage declares (nak_nir.c, nir_lower_tess_coord_z).

const OP_ENTRY_POINT: u32 = 15;
const OP_EXECUTION_MODE: u32 = 16;
const MODEL_TESSELLATION_CONTROL: u32 = 1;
const MODEL_TESSELLATION_EVALUATION: u32 = 2;
const HEADER_WORDS: usize = 5;

const MODE_SPACING_EQUAL: u32 = 1;
const MODE_SPACING_FRACTIONAL_EVEN: u32 = 2;
const MODE_SPACING_FRACTIONAL_ODD: u32 = 3;
const MODE_VERTEX_ORDER_CW: u32 = 4;
const MODE_VERTEX_ORDER_CCW: u32 = 5;
const MODE_POINT_MODE: u32 = 10;
const MODE_TRIANGLES: u32 = 22;
const MODE_QUADS: u32 = 24;
const MODE_ISOLINES: u32 = 25;
const TESSELLATION_MODES: [u32; 9] = [
    MODE_SPACING_EQUAL,
    MODE_SPACING_FRACTIONAL_EVEN,
    MODE_SPACING_FRACTIONAL_ODD,
    MODE_VERTEX_ORDER_CW,
    MODE_VERTEX_ORDER_CCW,
    MODE_POINT_MODE,
    MODE_TRIANGLES,
    MODE_QUADS,
    MODE_ISOLINES,
];

/// The instructions of a module: (opcode, its words), after the header
fn instructions(words: &[u32]) -> impl Iterator<Item = (u32, &[u32])> {
    let mut at = HEADER_WORDS.min(words.len());
    std::iter::from_fn(move || {
        let head = *words.get(at)?;
        let count = (head >> 16) as usize;
        if count == 0 || at + count > words.len() {
            return None;
        }
        let instruction = &words[at..at + count];
        at += count;
        Some((head & 0xffff, instruction))
    })
}

/// The entry point id of the module's first entry of `model`
fn entry_of(words: &[u32], model: u32) -> Option<u32> {
    instructions(words)
        .find(|(op, i)| *op == OP_ENTRY_POINT && i.len() > 2 && i[1] == model)
        .map(|(_, i)| i[2])
}

/// The tessellation modes the control stage's module declares for its entry point
pub(crate) fn control_modes(control: &[u32]) -> Vec<u32> {
    let Some(entry) = entry_of(control, MODEL_TESSELLATION_CONTROL) else {
        return Vec::new();
    };
    instructions(control)
        .filter(|(op, i)| *op == OP_EXECUTION_MODE && i.len() == 3 && i[1] == entry)
        .map(|(_, i)| i[2])
        .filter(|m| TESSELLATION_MODES.contains(m))
        .collect()
}

/// The evaluation stage's module with `modes` declared for its entry point where it declares none of them itself; None where
/// it has no evaluation entry point
pub(crate) fn evaluation_with_modes(evaluation: &[u32], modes: &[u32]) -> Option<Vec<u32>> {
    let entry = entry_of(evaluation, MODEL_TESSELLATION_EVALUATION)?;
    let declared: Vec<u32> = instructions(evaluation)
        .filter(|(op, i)| *op == OP_EXECUTION_MODE && i.len() == 3 && i[1] == entry)
        .map(|(_, i)| i[2])
        .collect();
    let mut after_entries = HEADER_WORDS;
    let mut at = HEADER_WORDS;
    for (op, i) in instructions(evaluation) {
        at += i.len();
        if op == OP_ENTRY_POINT {
            after_entries = at;
        }
    }
    let mut out = evaluation[..after_entries].to_vec();
    for &mode in modes.iter().filter(|m| !declared.contains(m)) {
        out.extend_from_slice(&[3 << 16 | OP_EXECUTION_MODE, entry, mode]);
    }
    out.extend_from_slice(&evaluation[after_entries..]);
    Some(out)
}

/// SET_TESSELLATION_PARAMETERS as NVK_MME_SET_TESS_PARAMS makes it of the modes (nvk_cmd_draw.c nvk_mme_set_tess_params): the
/// domain, the spacing, and the output primitives (points, lines, or triangles in the order the control stage names, for
/// Vulkan's default upper-left domain origin); 0 where the modes name no domain
pub(crate) fn parameters(modes: &[u32]) -> u32 {
    let has = |m: u32| modes.contains(&m);
    let domain = if has(MODE_TRIANGLES) {
        1
    } else if has(MODE_QUADS) {
        2
    } else if has(MODE_ISOLINES) {
        0
    } else {
        return 0;
    };
    let spacing = if has(MODE_SPACING_FRACTIONAL_ODD) {
        1
    } else if has(MODE_SPACING_FRACTIONAL_EVEN) {
        2
    } else {
        0
    };
    let primitives = if has(MODE_POINT_MODE) {
        0
    } else if domain == 0 {
        1
    } else if has(MODE_VERTEX_ORDER_CCW) {
        3
    } else {
        2
    };
    domain | spacing << 4 | primitives << 8
}

#[cfg(test)]
mod tests {
    use super::*;

    fn module(entry_model: u32, modes: &[u32]) -> Vec<u32> {
        let mut words = vec![0x0723_0203, 0x0001_0000, 0, 100, 0];
        words.extend_from_slice(&[4 << 16 | OP_ENTRY_POINT, entry_model, 7, 0x6e69_616d]);
        for &m in modes {
            words.extend_from_slice(&[3 << 16 | OP_EXECUTION_MODE, 7, m]);
        }
        words.extend_from_slice(&[2 << 16 | 19, 9]);
        words
    }

    #[test]
    fn evaluation_takes_the_control_stage_modes() {
        let control = module(
            MODEL_TESSELLATION_CONTROL,
            &[MODE_TRIANGLES, MODE_SPACING_FRACTIONAL_ODD, MODE_VERTEX_ORDER_CCW, 26],
        );
        let modes = control_modes(&control);
        assert_eq!(
            modes,
            vec![MODE_TRIANGLES, MODE_SPACING_FRACTIONAL_ODD, MODE_VERTEX_ORDER_CCW]
        );
        let evaluation = module(MODEL_TESSELLATION_EVALUATION, &[]);
        let patched = evaluation_with_modes(&evaluation, &modes).expect("an evaluation entry");
        let declared: Vec<u32> = instructions(&patched)
            .filter(|(op, _)| *op == OP_EXECUTION_MODE)
            .map(|(_, i)| i[2])
            .collect();
        assert_eq!(declared, modes);
        assert_eq!(patched.len(), evaluation.len() + 9);
        assert_eq!(patched[patched.len() - 2..], evaluation[evaluation.len() - 2..]);
    }

    #[test]
    fn parameters_follow_nvk() {
        assert_eq!(
            parameters(&[MODE_TRIANGLES, MODE_SPACING_EQUAL, MODE_VERTEX_ORDER_CW]),
            0x201
        );
        assert_eq!(
            parameters(&[MODE_QUADS, MODE_SPACING_FRACTIONAL_EVEN, MODE_VERTEX_ORDER_CCW]),
            0x322
        );
        assert_eq!(
            parameters(&[MODE_TRIANGLES, MODE_SPACING_FRACTIONAL_ODD, MODE_POINT_MODE]),
            0x011
        );
        assert_eq!(parameters(&[MODE_SPACING_EQUAL]), 0);
    }
}
