#include "NativeHidTouchPlan.h"
_Static_assert(NHT_REPORT_BYTES==62,"wire size changed");
int pure_header_c_smoke(void){struct nht_state a={0};struct nht_plan p;return nht_build(&a,0,0,2,&p);}
