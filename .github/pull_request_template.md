## What this changes

<!-- One or two sentences. What did you do, and why? -->

## How you know it works

<!-- Which tests cover this? If hardware was involved, say what you saw. -->

## Checklist

- [ ] `west twister -p native_sim -T tests` passes locally
- [ ] New or changed behavior has a test
- [ ] No `#ifdef CONFIG_BOARD_NATIVE_SIM` in driver or application code
- [ ] Devicetree changes are in an overlay, not hardcoded
