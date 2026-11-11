// The core's plain vector and quaternion (their maths: vec_math.h).
#pragma once

namespace delirium
{
	struct V3
	{
		float x{}, y{}, z{};
	};
	struct Q
	{
		float x{}, y{}, z{}, w{ 1.0f };
	};
}
