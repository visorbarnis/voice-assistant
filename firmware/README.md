This directory stores the latest prebuilt firmware artifacts for `run_upload_firmware.sh`.

The script updates these files after a successful build:
- `bootloader.bin`
- `partitions.bin`
- `firmware.bin`
- `srmodels.bin`
- `manifest.env`

`settings.bin` is intentionally not stored here. It is rebuilt from current `settings.csv`
on each `run_upload_firmware.sh` launch and flashed directly to NVS.
