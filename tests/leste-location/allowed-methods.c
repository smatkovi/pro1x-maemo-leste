/*
 * Prints location_gpsd_control_get_allowed_methods(), which is
 * liblocation's get_selected_method(control, 0): the methods
 * LOCATION_METHOD_USER_SELECTED stands for with GConf's settings
 * (gps-disabled, network-disabled). Unfixed liblocation gives 0 whatever
 * they are; fixed, CWP|ACWP|GNSS|AGNSS (15) with both allowed, GNSS (4)
 * with the network off, CWP|ACWP (3) with GPS off, 0 with both off.
 *
 *   gcc -o allowed-methods allowed-methods.c $(pkg-config --cflags --libs liblocation)
 */
#include <stdio.h>
#include <location/location-gpsd-control.h>

int main(void)
{
	LocationGPSDControl *control = location_gpsd_control_get_default();

	printf("allowed methods: %d\n",
	    location_gpsd_control_get_allowed_methods(control));
	g_object_unref(control);
	return 0;
}
