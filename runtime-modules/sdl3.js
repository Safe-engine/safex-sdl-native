const sdl3 = globalThis.__safexNativeModules.sdl3

export const isNative = true
export const {
  createWindow, getViewportMetrics, getWinSize, loadTextFile, loadBinaryFile,
  loadTexture, loadFont, loadTextTexture, releaseTexture, releaseFont,
  getTextureWidth, getTextureHeight, loadAudio, releaseAudio, playAudio,
  stopAudio, pauseAudio, resumeAudio, setAudioVolume, isAudioPlaying,
  updateAudio, clear, submitCommandBuffer, present, getRendererStats,
  onInit, onUpdate, onRender, onTouchStart, onTouchMove, onTouchEnd,
  onTextInput, onKeyDown, onKeyUp, startTextInput, stopTextInput, onPause,
  onResume, onBackground, onForeground, onInterruption, onLowMemory,
  onOrientationChange, onTerminate,
} = sdl3
