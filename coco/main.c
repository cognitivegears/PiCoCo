#include <cmoc.h>
#include "dw.h"

static char buf[1024];
static u8 sec[256];

int main(void)
{
    char *body;
    int rc = picoco_cmd("version", buf, sizeof buf, &body);
    printf("VERSION RC %d: %s\n", rc, rc >= 0 ? body : "");
    rc = picoco_cmd("dw disk show", buf, sizeof buf, &body);
    printf("DISKS RC %d: %s\n", rc, rc >= 0 ? body : "");
    rc = picoco_cmd("smoke", buf, sizeof buf, &body);
    printf("SMOKE RC %d: %s\n", rc, rc > 0 ? body : "");
    rc = dw_read_sector(0, 0, sec);
    printf("READ RC %d\n", rc);
    return 0;
}
