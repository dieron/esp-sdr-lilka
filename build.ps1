# Rebuild firmware\esp-sdr-lilka.bin.
#
# Builds happen in C:\esp (short path, not in a synced folder: they
# corrupt git object stores and long paths break Windows builds). If the
# working copies are missing, this recreates them from the pinned commits and
# applies patches\esp-sdr-lilka.patch.
param(
    [string]$Root = 'C:\esp',
    # Screen rows the UI may use. 196 (default) keeps clear of a panel whose
    # bottom lines are dead, like the development Lilka's; 240 = all.
    [int]$UsedHeight = 196,
    [string]$Output = 'firmware\esp-sdr-lilka.bin'
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$sdr = Join-Path $Root 'esp-sdr'
$idf = Join-Path $Root 'idf'
$sdrCommit = '89193f1190dd0f1f105e69bf29d2699ed8bf48a9'  # ESPARGOS/esp-sdr main, 2026-11-08
$idfCommit = '25fe69f946311abdaf9ad56591f25fedbc20ac98'  # pinned by esp-sdr firmware-targets.json (IDF 6.2-dev)

if (-not (Test-Path $idf)) {
    git init -q $idf
    git -C $idf remote add origin https://github.com/espressif/esp-idf.git
    git -C $idf fetch -q --depth 1 origin $idfCommit
    git -C $idf checkout -q FETCH_HEAD
    git -C $idf submodule update --init --recursive --depth 1 --jobs 8
    & "$idf\install.ps1" esp32s3
}
if (-not (Test-Path $sdr)) {
    git clone https://github.com/ESPARGOS/esp-sdr $sdr
    git -C $sdr checkout -q -b lilka $sdrCommit
    git -C $sdr submodule update --init --depth 1
    git -C $sdr apply --whitespace=nowarn (Join-Path $here 'patches\esp-sdr-lilka.patch')
}

. "$idf\export.ps1" | Out-Null
$env:IDF_COMPONENT_MANAGER = '0'
Push-Location $sdr
try {
    idf.py -B build-lilka -DIDF_TARGET=esp32s3 '-DSDKCONFIG=build-lilka/sdkconfig' `
        '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults.esp32s3;sdkconfig.defaults.lilka' `
        -DESP_SDR_LILKA=ON "-DLILKA_LCD_USED_HEIGHT=$UsedHeight" `
        -DRING_PROBE=OFF -DSAMPLE_RATE_PROBE=OFF -DFILTER_REGISTER_PROBE=OFF -DS3_RF_PROBE=OFF build
    if ($LASTEXITCODE) { throw "idf.py failed ($LASTEXITCODE)" }
    $out = Join-Path $here $Output
    New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
    Copy-Item build-lilka\esp_sdr.bin $out -Force
    Copy-Item main\lilka\* (Join-Path $here 'src\main\lilka') -Force
    git add -N main/lilka sdkconfig.defaults.lilka
    # Let git write the file: piping through PowerShell rewrites line endings.
    git diff --binary --output=(Join-Path $here 'patches\esp-sdr-lilka.patch') $sdrCommit -- .
    Write-Host "$Output updated ($UsedHeight rows)"
} finally {
    Pop-Location
}
