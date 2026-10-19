#ifndef PORT_SETTING_LIST_H
#define PORT_SETTING_LIST_H

// The values a numeric setting offers in the settings menu (port_settings.h):
// the standard steps, plus the value the game started with when that is not
// one of them (SMS_VOLUME=33, say), put in its place so the menu always shows
// the value in force. Built on first use, from the value then in force, which
// is still the launcher's: the menu cannot have changed anything before it
// asked for the list. C++ only; every module that needs one keeps its own
// static PortValueList (zero-initialised, so `built` starts false).
struct PortValueList {
	enum { kMax = 40 };
	int v[kMax];
	int n;
	bool built;

	// `steps` ascending; `start` is the value in force.
	void build(const int* steps, int count, int start)
	{
		n           = 0;
		bool placed = false;
		for (int i = 0; i < count && n < kMax - 1; i++) {
			if (!placed && start < steps[i]) {
				v[n++] = start;
				placed = true;
			}
			if (start == steps[i])
				placed = true;
			v[n++] = steps[i];
		}
		if (!placed)
			v[n++] = start;
		built = true;
	}

	// The entry equal to `value`, else the nearest one.
	int indexOf(int value) const
	{
		int best = 0, bestDist = 0x7FFFFFFF;
		for (int i = 0; i < n; i++) {
			const int d = v[i] > value ? v[i] - value : value - v[i];
			if (d < bestDist) {
				best     = i;
				bestDist = d;
			}
		}
		return best;
	}
};

#endif
