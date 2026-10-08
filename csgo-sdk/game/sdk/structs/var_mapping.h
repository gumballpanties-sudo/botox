#pragma once

struct var_mapping_entry {
public:
	unsigned short type;

	bool needs_to_interpolate;

	void* data;
	void* watcher;
};

struct var_mapping {
public:
	var_mapping_entry* entries;

	int interpolated_entries;
	float last_interpolation_time;
};
