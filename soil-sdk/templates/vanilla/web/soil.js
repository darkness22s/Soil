if (!window.soil) {
  throw new Error("window.soil is missing; run this app with `soil dev` or the Soil runtime");
}

export const soil = window.soil;
