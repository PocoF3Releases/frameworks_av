# Optional legacy Dolby DAP integration

Disabled unless the product explicitly enables the framework integration in
BoardConfig:

```make
$(call soong_config_set_bool,audio,legacy_dap_integration,true)
```

The default is false. Existing Dolby properties alone cannot activate this
integration, including on products with newer DAP versions, Codec2 decoders,
or another OEM's audio service. This switch does not change Codec2 or codecs.

An opted-in product must also set all three read-only properties:

- `ro.vendor.audio.dolby.dax.support=true`
- `ro.vendor.audio.dolby.dax.version=DAX3_3.6` (or a dot/underscore-qualified suffix)
- `ro.vendor.audio.dolby.dap.control=qdsp` or `legacy`

These select an effect ABI, not an OEM, kernel version, or audio HAL version.
Only the output-mix DAP type `46d279d9-9be7-453d-9d7c-ef937f675587`
and implementation `9d4921da-8225-4f29-aefa-39537a04bcaa` are controlled.
Missing/unknown properties and other implementations receive no private commands.
Product policy must permit audioserver to read these vendor properties.

`qdsp` uses standard EFFECT_CMD_OFFLOAD for I/O attachment and scalar U8.24
pregain parameter 0x10. `legacy` selects the separate private I/O/audio-flags
and two-word pregain contract; do not select it merely from a version string.
Both require validation against the actual effect binary before enabling.

The passive AudioPolicyService owner is limited to `qdsp` DAX 3.6. It uses
CPDP parameter 5 to mirror the native enable state only while it owns control;
it does not replace the higher-priority settings app. The controller serializes
commands with effect-chain lifetime and bounds failed-command retries.

Alioth selects `qdsp` in its common device configuration. Other products keep
upstream playback behavior unless they explicitly opt in. No kernel capability
is inferred and this integration does not require debugfs or a kernel backport.
