#pragma once

class GfxRenderer {
 public:
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };

  Orientation getOrientation() const { return orientation; }

  Orientation orientation = Portrait;
};
