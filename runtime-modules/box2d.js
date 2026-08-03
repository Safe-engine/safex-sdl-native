const box2d = globalThis.__safexNativeModules.box2d

export const {
  createWorld, destroyWorld, stepWorld, setGravity, createBody, destroyBody,
  createBoxShape, createCircleShape, createPolygonShape, createSegmentShape,
  getBodyTransform, setBodyTransform, setLinearVelocity, applyForceToCenter,
  applyLinearImpulseToCenter, getContactEvents, getDebugDraw,
} = box2d
