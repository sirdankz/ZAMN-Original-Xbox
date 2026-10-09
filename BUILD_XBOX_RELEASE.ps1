param(
    [ValidateSet("Original","Enhanced","Remastered")]
    [string]$AudioMode = "Enhanced",
    [ValidateSet("Balanced","Punchy","Wide")]
    [string]$EnhancedPreset = "Wide",
    [string]$RemasteredBankPath = "",
    [switch]$SyntaxCheck,
    [switch]$NoExplorer
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

$rxdkRoot = "C:\ProgramData\RXDK"
$cli = Join-Path $rxdkRoot "tools\Rxdk.Cli.exe"
$sdk = Join-Path $rxdkRoot "sdk"
$llvm = Join-Path $rxdkRoot "llvm\xboxog-windows-x64"
$xdvdfs = Join-Path $rxdkRoot "tools\xdvdfs.exe"
$manifest = Join-Path $root "rxdk.project.json"
$rom = Get-ChildItem $root -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -in @('.sfc','.smc') } |
    Sort-Object @{Expression={if($_.Name -match 'Zombies.*Neighbors'){0}else{1}}}, @{Expression='Length';Descending=$true} |
    Select-Object -First 1

if (!(Test-Path $cli)) { throw "RXDK CLI not found: $cli" }
if (!(Test-Path (Join-Path $sdk 'include\xtl.h'))) { throw "RXDK SDK not found: $sdk" }
if (!(Test-Path (Join-Path $llvm 'bin\clang.exe'))) { throw "RXDK LLVM not found: $llvm" }
if (!(Test-Path $manifest)) { throw "rxdk.project.json not found" }
foreach ($required in @(
    'src\native\runtime.c','src\native\audio.c','src\native\video.c',
    'src\netplay\netplay_core.c','src\platform\xbox\xbox_netplay.cpp',
    'src\platform\xbox\xbox_main.cpp')) {
    if (!(Test-Path (Join-Path $root $required))) { throw "Required source missing: $required" }
}
if (!$rom -and !$SyntaxCheck) { throw "ROM not found in project root. Add your own Zombies Ate My Neighbors .sfc/.smc; ROMs are not included." }

# The manual is optional and supplied locally by the user.
$manualConversionOK=$true
if (!$SyntaxCheck -and ((Test-Path -LiteralPath (Join-Path $root 'manual\Zombies Ate My Neighbors.pdf')) -or
    (Test-Path -LiteralPath (Join-Path $root 'manual\Zombies Ate My Neighbors.epub')))) {
    $python=Get-Command python -ErrorAction SilentlyContinue
    $pythonArgs=@()
    if (!$python) {
        $python=Get-Command py -ErrorAction SilentlyContinue
        $pythonArgs=@('-3')
    }
    if ($python) {
        $savedErrorAction=$ErrorActionPreference
        try {
            $ErrorActionPreference='Continue'
            & $python.Source @pythonArgs (Join-Path $root 'tools\convert_manual.py') 2>&1 | ForEach-Object { Write-Host "$_" }
            $manualConversionOK=($LASTEXITCODE -eq 0)
        } catch {
            $manualConversionOK=$false
            Write-Warning "Manual omitted: Python could not run. $_"
        } finally {
            $ErrorActionPreference=$savedErrorAction
        }
    } else {
        $manualConversionOK=$false
        Write-Warning 'Manual omitted: install Python 3 and tools/manual-requirements.txt to convert the PDF/EPUB.'
    }
}
$manualAssets=@()
$manualComplete=$manualConversionOK
for ($page=0; $page -lt 10; $page++) {
    $asset=Join-Path $root ('manual\spread{0:D2}.rgb565' -f $page)
    if (!(Test-Path -LiteralPath $asset) -or (Get-Item -LiteralPath $asset).Length -ne 1556480) {
        $manualComplete=$false
        break
    }
    $manualAssets += $asset
}

$env:RXDK_STAGED_SDK = $sdk
$env:RXDK_LLVM = $llvm
$env:RXDK_STAGED_TOOLS = Join-Path $rxdkRoot "tools"

$utf8 = New-Object System.Text.UTF8Encoding($false)
$originalManifest = [IO.File]::ReadAllText($manifest)
try {
    $j = $originalManifest | ConvertFrom-Json
    $rel = $j.configurations.Release

    $sources = @($rel.sources)
    foreach ($src in @('src\native\runtime.c','src\native\audio.c','src\native\video.c','src\netplay\netplay_core.c','src\platform\xbox\xbox_netplay.cpp')) {
        if ($sources -notcontains $src) { $sources += $src }
    }
    $rel.sources = $sources

    # Strip all historical/profile selections and assemble one production configuration.
    $defs = @($rel.defines | Where-Object {
        $_ -notmatch '^ZAMN_R[0-9]' -and
        $_ -ne 'ZAMN_STOCK_CORE=1' -and $_ -ne 'PORT_COVERAGE=1' -and
        $_ -ne 'ZAMN_RELEASE_NO_DIAGNOSTICS=1' -and
        $_ -ne 'ZAMN_R39_BUFFERED_LOG=1' -and $_ -ne 'ZAMN_R499_SERVICE_LOG=1' -and
        $_ -ne 'ZAMN_R67_GUARD_REASON_PROFILE=1' -and
        $_ -ne 'ZAMN_R69_PORTING_PROFILE=1' -and
        $_ -ne 'ZAMN_R70_PAUSE_TRACE=1' -and
        $_ -ne 'ZAMN_R70_DIAGNOSTIC_OAM_PARITY=1' -and
        $_ -notmatch '^ZAMN_R41_AUDIO_DEFAULT=' -and
        $_ -notmatch '^ZAMN_R421_AUDIO_PROFILE='
    })
    foreach ($d in @(
        'COSIM_FAST_RUNTIME=1',
        'ZAMN_R15_THINLTO=1',
        'ZAMN_R23_MMX_PPU=1',
        'ZAMN_R24_RELEASE_FAST=1',
        'ZAMN_R28_NATIVE_BULK_BURN=1',
        'ZAMN_R31_MMX_TRANSPARENT_EARLYOUT=1',
        'ZAMN_R32_PPU_HOISTS=1',
        'ZAMN_R33_TRUE_NATIVE_CUTOVER=1',
        'ZAMN_R33_RELEASE_LEAN=1',
        'ZAMN_R34_TRUE_NATIVE_CUTOVER=1',
        'ZAMN_R35_NATIVE_OBJECT_PIPELINE=1',
        'ZAMN_R36_NATIVE_LEAN=1',
        'ZAMN_R37_READONLY_GUARDS=1',
        'ZAMN_R38_READONLY_WALK_GUARD=1',
        'ZAMN_RELEASE_NO_DIAGNOSTICS=1',
        'ZAMN_R40_FAST_OAM_PREFLIGHT=1',
        'ZAMN_R41_TRANSITION_WAIT_CUTOVER=1',
        'ZAMN_R41_NATIVE_RENDER_OWNER=1',
        'ZAMN_R41_NATIVE_AUDIO=1',
        'ZAMN_R42_LEVEL_INTRO_WAIT_CUTOVER=1',
        'ZAMN_R43_NATIVE_WAIT_CUTOVER=1',
        'ZAMN_R44_NATIVE_BURN_ATTRIB=1',
        'ZAMN_R45_APU_SET_NATIVE_TRANSFER=1',
        'ZAMN_R46_APU_COMPACT_TRACE=1',
        'ZAMN_R46_STRICT_NATIVE_SHARE=1',
        'ZAMN_R47_NETPLAY=1',
        'ZAMN_R48_ROLLBACK=1',
        'ZAMN_R50_NATIVE_HOTPATHS=1',
        'ZAMN_R51_NATIVE_HOTPATHS=1',
        'ZAMN_R52_FAST_COLLISION_GUARDS=1',
        'ZAMN_R53_NATIVE_MOVEMENT=1',
        'ZAMN_R54_NATIVE_D9_CLUSTER=1',
        'ZAMN_R55_NATIVE_ACTOR_FRAGMENTS=1',
        'ZAMN_R56_SAFE_OAM_CUTOVER=1',
        'ZAMN_R57_OAM_COLLISION_PROOF=1',
        'ZAMN_R58_COLLISION_THREAD_FASTPATH=1',
        'ZAMN_R59_NATIVE_COLLISION_FRAGMENTS=1',
        'ZAMN_R60_CONNECTED_NATIVE=1',
        'ZAMN_R61_THREAD_ACTOR_NATIVE=1',
        'ZAMN_R62_ACTOR_CONTROL_NATIVE=1',
        'ZAMN_R63_POSTCALL_NATIVE=1',
        'ZAMN_R64_ACTOR_RECORD_NATIVE=1',
        'ZAMN_R65_HOT_NATIVE_DISPATCH=1',
        'ZAMN_R66_TOTAL_HANDLER_FAST_GUARDS=1',
        'ZAMN_R68_INPUT_READONLY_GUARDS=1',
        'ZAMN_R70_HOT_THREAD_9A6D=1',
        'ZAMN_R74_COLLISION_OAM_INPUT_PROOF=1',
        'ZAMN_R71_CONTROL_MAPPING=1',
        'ZAMN_R76_LT_FALLBACK_FIX=1',
        'ZAMN_R421_ENHANCED_PRESETS=1'
    )) { if ($defs -notcontains $d) { $defs += $d } }

    $audioDefault = switch ($AudioMode) { 'Remastered' { 2 } 'Enhanced' { 1 } default { 0 } }
    $audioProfile = if ($AudioMode -eq 'Enhanced') {
        switch ($EnhancedPreset) { 'Punchy' { 1 } 'Wide' { 2 } default { 0 } }
    } else { 0 }
    $defs += "ZAMN_R41_AUDIO_DEFAULT=$audioDefault"
    $defs += "ZAMN_R421_AUDIO_PROFILE=$audioProfile"
    $rel.defines = $defs

    $flags = @()
    if ($rel.PSObject.Properties.Name -contains 'compileFlags') { $flags = @($rel.compileFlags) }
    $wanted = @('-flto=thin','-fno-semantic-interposition','-fomit-frame-pointer','-mtune=pentium3','-mmmx','-msse','-mno-sse2')
    $flags = @($flags | Where-Object { $wanted -notcontains $_ })
    $flags += $wanted
    if ($rel.PSObject.Properties.Name -contains 'compileFlags') { $rel.compileFlags = $flags }
    else { $rel | Add-Member -NotePropertyName compileFlags -NotePropertyValue $flags }
    if ($rel.PSObject.Properties.Name -contains 'imageBuild' -and $rel.imageBuild) { $rel.imageBuild.debug = $false }

    [IO.File]::WriteAllText($manifest, ($j | ConvertTo-Json -Depth 20), $utf8)

    Write-Host '=== ZAMN R79 PUBLIC RELEASE / NO RUNTIME LOGGING ===' -ForegroundColor Cyan
    Write-Host 'Runtime logging and profiling are disabled.' -ForegroundColor Yellow
    Write-Host 'Build 0x5A4D490A, protocol 4. Netplay settings and behavior unchanged.' -ForegroundColor Yellow
    Write-Host "Deterministic state hashes, rollback safety, instant canonical start, and start-transition audio mute remain enabled." -ForegroundColor Yellow
    Write-Host "Audio: $AudioMode$(if($AudioMode -eq 'Enhanced'){" / $EnhancedPreset"})" -ForegroundColor Yellow

    if ($SyntaxCheck) {
        $clang = Join-Path $llvm 'bin\clang.exe'
        $resourceDir = & $clang -print-resource-dir
        $argsCommon = @('--target=i686-pc-windows-gnu','-march=pentium3','-fms-extensions',
            '-fms-compatibility','-ffreestanding','-fno-stack-protector','-femulated-tls',
            '-O3','-fno-sanitize=undefined','-U_DEBUG','-D__ASSERT_VERBOSE',
            '-Wno-unused-command-line-argument','-Wno-pragma-pack',
            '-fno-builtin','-nostdinc','-include',(Join-Path $sdk 'include\picolibc.h'),
            '-I',(Join-Path $sdk 'include'),'-isystem',(Join-Path $resourceDir 'include'),'-fsyntax-only')
        foreach ($inc in $rel.includePaths) { $argsCommon += @('-I',(Join-Path $root $inc)) }
        foreach ($d in $defs) { $argsCommon += "-D$d" }
        $argsCommon += $flags
        foreach ($src in @('src\native\runtime.c','src\platform\xbox\xbox_main.cpp','src\platform\xbox\xbox_audio.cpp','src\platform\xbox\xbox_netplay.cpp','src\cosim\routines.c','src\port\player_resume.c','src\port\score.c','src\port\hotpaths.c','third_party\lakesnes\snes\statehandler.c')) {
            [string[]]$languageArgs = if ($src.EndsWith('.cpp')) {
                @('-std=c++23','-nostdinc++','-fexceptions','-frtti','-D_LIBCPP_ENABLE_CXX17_REMOVED_AUTO_PTR','-fms-compatibility-version=19.20','-U_WIN32','-U__MINGW32__','-D_GNU_SOURCE','-I',(Join-Path $sdk 'include\c++\v1'))
            } else { @('-std=c23') }
            & $clang @argsCommon @languageArgs (Join-Path $root $src)
            if ($LASTEXITCODE -ne 0) { throw "Release syntax check failed: $src" }
        }
        Write-Host 'Release Xbox syntax checks passed.' -ForegroundColor Green
        return
    }

    function Remove-BuildOutput([string]$path) {
        $resolved = [IO.Path]::GetFullPath($path)
        $allowed = [IO.Path]::GetFullPath((Join-Path $root 'out')) + [IO.Path]::DirectorySeparatorChar
        if (!$resolved.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe build output: $resolved" }
        if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
    }

    Remove-BuildOutput (Join-Path $root 'out\Release')
    & $cli build --project-root $root --configuration Release
    if ($LASTEXITCODE -ne 0) { throw "RXDK compile failed with exit code $LASTEXITCODE" }

    $xbe = Get-ChildItem (Join-Path $root 'out\Release') -Recurse -File -Filter '*.xbe' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (!$xbe) { throw "No XBE produced" }

    $tag = 'R49_9_7_RELEASE_' + $AudioMode.ToUpperInvariant()
    if ($AudioMode -eq 'Enhanced') { $tag += '_' + $EnhancedPreset.ToUpperInvariant() }
    $outRoot = Join-Path $root 'out'
    $dest = Join-Path $outRoot 'Zombies Ate My Neighbors!'
    Remove-BuildOutput $dest
    New-Item $dest -ItemType Directory -Force | Out-Null
    Copy-Item $xbe.FullName (Join-Path $dest 'default.xbe') -Force
    Copy-Item $rom.FullName (Join-Path $dest 'Zombies Ate My Neighbors.sfc') -Force
    if ($manualComplete) {
        $manualOut=Join-Path $dest 'manual'
        New-Item $manualOut -ItemType Directory -Force | Out-Null
        foreach ($asset in $manualAssets) {
            Copy-Item -LiteralPath $asset -Destination $manualOut -Force
        }
        Write-Host 'Optional game manual copied.' -ForegroundColor Green
    } else {
        Write-Host 'Manual omitted: provide all ten valid spreads to enable GAME MANUAL.' -ForegroundColor Yellow
    }

    if ($AudioMode -eq 'Enhanced') {
        [IO.File]::WriteAllBytes((Join-Path $dest 'audio_enhanced.flag'), [byte[]]@())
    } elseif ($AudioMode -eq 'Remastered') {
        [IO.File]::WriteAllBytes((Join-Path $dest 'audio_remastered.flag'), [byte[]]@())
        $bank = $null
        if ($RemasteredBankPath) {
            $candidate = if ([IO.Path]::IsPathRooted($RemasteredBankPath)) { $RemasteredBankPath } else { Join-Path $root $RemasteredBankPath }
            if (!(Test-Path $candidate)) { throw "Remastered bank not found: $candidate" }
            $bank = Get-Item $candidate
        } elseif (Test-Path (Join-Path $root 'zamn-remastered.zsb')) {
            $bank = Get-Item (Join-Path $root 'zamn-remastered.zsb')
        }
        if ($bank) { Copy-Item $bank.FullName (Join-Path $dest 'zamn-remastered.zsb') -Force }
        else { Write-Warning 'Remastered mode built without a ZSB1 bank; missing PCM entries fall back to original BRR samples.' }
    }

    if (Test-Path $xdvdfs) {
        $iso = Join-Path $outRoot ("zamn_" + $tag + ".iso")
        & $xdvdfs pack $dest $iso
        if ($LASTEXITCODE -ne 0) { Write-Warning 'XISO packing failed' }
        elseif (Test-Path $iso) { Write-Host "ISO: $iso" -ForegroundColor Green }
    }
    Write-Host "XBE: $(Join-Path $dest 'default.xbe')" -ForegroundColor Green

    if (!$NoExplorer) { Start-Process explorer.exe $outRoot }
}
finally {
    [IO.File]::WriteAllText($manifest, $originalManifest, $utf8)
}
