# The spec files the ladder is measured through, generated so that the column
# ORDER and the iteration counts cannot drift between the five rows.
#
#   python bench/tools/ladder_specs.py <output-dir>
#
# Writes spec_env.json, spec_m4.json, spec_nl.json, spec_ms.json and
# spec_node.json beside the binaries `ladder.sh` built there.

import json
import os
import sys

TAGS = [
    ("s34", "stage 3.4"),
    ("e1", "E1"),
    ("e2", "E2"),
    ("e3", "E3"),
    ("e4", "E4 shipped"),
    ("e5", "E5"),
    ("b1", "B1 shipped"),
]

# fixture -> (big source, small source, iterations the delta covers)
TWO_COUNT = {
    "es": ("env_slot_kernel.js", "es_small.js", 5_400_000),
    "m4": ("mat4_kernel.js", "mat4_kernel_small.js", 18_000_000),
    "nl": ("nullish_pin_kernel.js", "nl_small.js", 7_200_000),
}
MS = ["typed_array_crunch", "three_math", "mesh_churn_2k", "object_graph"]


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    for key, (_, _, iters) in TWO_COUNT.items():
        spec = []
        for tag, label in TAGS:
            spec.append({"name": label, "exe": f"./{key}_{tag}_b.exe",
                         "small": f"{label} twin", "iters": iters})
            spec.append({"name": f"{label} twin", "exe": f"./{key}_{tag}_s.exe"})
        json.dump(spec, open(os.path.join(out, f"spec_{key}.json"), "w"), indent=1)

    spec = []
    for fixture in MS:
        for tag, label in TAGS:
            spec.append({"name": f"{fixture} {label}", "exe": f"./{fixture}_{tag}.exe"})
    json.dump(spec, open(os.path.join(out, "spec_ms.json"), "w"), indent=1)

    # node, out of band and in the same session: the two-count kernels plus the
    # four millisecond fixtures.
    node = [
        {"name": "env_slot", "js": "../env_slot_kernel.js", "small": "env_slot twin",
         "iters": 5_400_000},
        {"name": "env_slot twin", "js": "./es_small.js"},
        {"name": "mat4", "js": "../mat4_kernel.js", "small": "mat4 twin", "iters": 18_000_000},
        {"name": "mat4 twin", "js": "../mat4_kernel_small.js"},
        {"name": "nullish", "js": "../nullish_pin_kernel.js", "small": "nullish twin",
         "iters": 7_200_000},
        {"name": "nullish twin", "js": "./nl_small.js"},
        {"name": "call_chain", "js": "../call_chain_kernel.js"},
    ] + [{"name": f, "js": f"../{f}.js"} for f in MS]
    json.dump(node, open(os.path.join(out, "spec_node.json"), "w"), indent=1)
    print("wrote specs to", out)


if __name__ == "__main__":
    main()
