$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Runtime.WindowsRuntime
[Windows.Data.Pdf.PdfDocument,Windows.Data.Pdf,ContentType=WindowsRuntime]|Out-Null
[Windows.Storage.StorageFile,Windows.Storage,ContentType=WindowsRuntime]|Out-Null
[Windows.Storage.StorageFolder,Windows.Storage,ContentType=WindowsRuntime]|Out-Null
[Windows.Storage.Streams.InMemoryRandomAccessStream,Windows.Storage.Streams,ContentType=WindowsRuntime]|Out-Null

$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
  $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
})[0]
$asTaskAction = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
  $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncAction'
})[0]

function Await($op,$type){
  $g=$asTaskGeneric.MakeGenericMethod($type)
  $t=$g.Invoke($null,@($op))
  $t.Wait(-1)|Out-Null
  $t.Result
}
function AwaitAction($action){
  $t=$asTaskAction.Invoke($null,@($action))
  $t.Wait(-1)|Out-Null
}

$pdfPath='C:\Users\dbc_1\Documents\PokeMotor2\docs\ray_tracing_gems_i.pdf'
$outDir='C:\Users\dbc_1\Documents\PokeMotor2\docs\gems_pages'
if(-not (Test-Path $outDir)){ New-Item -ItemType Directory $outDir|Out-Null }

$file = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($pdfPath)) ([Windows.Storage.StorageFile])
$doc  = Await ([Windows.Data.Pdf.PdfDocument]::LoadFromFileAsync($file)) ([Windows.Data.Pdf.PdfDocument])
$folder = Await ([Windows.Storage.StorageFolder]::GetFolderFromPathAsync($outDir)) ([Windows.Storage.StorageFolder])

Write-Output ("Total pages: " + $doc.PageCount)

$startPage = 495
$endPage = 525
for($i=$startPage; $i -le $endPage; $i++){
  $p = $doc.GetPage($i - 1)  # 0-indexed
  $name = "page_{0:D3}.png" -f $i
  $outFile = Await ($folder.CreateFileAsync($name, [Windows.Storage.CreationCollisionOption]::ReplaceExisting)) ([Windows.Storage.StorageFile])
  $stream = Await ($outFile.OpenAsync([Windows.Storage.FileAccessMode]::ReadWrite)) ([Windows.Storage.Streams.IRandomAccessStream])
  $opts = New-Object Windows.Data.Pdf.PdfPageRenderOptions
  $opts.DestinationHeight = 1800
  AwaitAction ($p.RenderToStreamAsync($stream, $opts))
  $stream.Dispose()
  Write-Output ("Rendered page $i -> $name")
}
Write-Output "DONE"