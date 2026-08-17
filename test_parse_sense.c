/*
 * test_parse_sense.c - table-driven tests for mega_progress.c's sense decoder.
 *
 * These tools cannot be exercised against a MegaRAID controller in CI, so the
 * sense parser - the one piece of real logic here - is tested against synthetic
 * REQUEST SENSE responses instead. Build and run via ./run_tests.sh, which
 * extracts parse_sense() straight out of mega_progress.c so the code under test
 * is literally the code that ships; there is no second copy to drift.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#define u8  uint8_t
#define u32 uint32_t

#include "parse_sense.inc"

static int failures;

static void check(const char *name, const u8 *buf, size_t len,
                  int exp_rc, int exp_key, int exp_asc, int exp_ascq, int exp_prog) {
    u8 key = 0xEE, asc = 0xEE, ascq = 0xEE;
    int prog = -99;
    int rc = parse_sense(buf, len, &key, &asc, &ascq, &prog);
    int ok = (rc == exp_rc) &&
             (rc != 0 || (key == exp_key && asc == exp_asc &&
                          ascq == exp_ascq && prog == exp_prog));

    printf("%-52s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) {
        failures++;
        printf("     got      rc=%d key=0x%x asc=0x%02x ascq=0x%02x prog=%d\n",
               rc, key, asc, ascq, prog);
        printf("     expected rc=%d key=0x%x asc=0x%02x ascq=0x%02x prog=%d\n",
               exp_rc, exp_key, exp_asc, exp_ascq, exp_prog);
    }
}

int main(void) {
    u8 b[96];

    /* ---- fixed format (what a conformant drive returns for DESC=0) ---- */

    memset(b, 0, sizeof b);
    b[0] = 0x70; b[2] = 0x02; b[7] = 0x0a; b[12] = 0x04; b[13] = 0x04;
    b[15] = 0x80; b[16] = 0x6b; b[17] = 0x84;
    check("fixed / NOT READY 04/04 / SKSV=1", b, sizeof b, 0, 0x2, 0x04, 0x04, 0x6b84);

    memset(b, 0, sizeof b);
    b[0] = 0x70; b[2] = 0x02; b[7] = 0x0a; b[12] = 0x04; b[13] = 0x04;
    b[15] = 0x00; b[16] = 0x6b; b[17] = 0x84;
    check("fixed / NOT READY 04/04 / SKSV=0 suppresses progress", b, sizeof b, 0, 0x2, 0x04, 0x04, -1);

    memset(b, 0, sizeof b);
    b[0] = 0x70; b[7] = 0x0a;
    check("fixed / NO SENSE, nothing reported", b, sizeof b, 0, 0x0, 0x00, 0x00, -1);

    /* A drive may format in the background while keeping the LU accessible,
       reporting NO SENSE with a valid progress field. Treating that as a
       finished format would tell an operator it is safe to power cycle. */
    memset(b, 0, sizeof b);
    b[0] = 0x70; b[2] = 0x00; b[7] = 0x0a;
    b[15] = 0x80; b[16] = 0x40; b[17] = 0x00;
    check("fixed / NO SENSE + SKSV progress is surfaced", b, sizeof b, 0, 0x0, 0x00, 0x00, 0x4000);

    /* Under ILLEGAL REQUEST the same bytes are a field pointer, not progress. */
    memset(b, 0, sizeof b);
    b[0] = 0x70; b[2] = 0x05; b[7] = 0x0a; b[12] = 0x24; b[13] = 0x00;
    b[15] = 0x80; b[16] = 0x00; b[17] = 0x01;
    check("fixed / ILLEGAL REQUEST field pointer is NOT progress", b, sizeof b, 0, 0x5, 0x24, 0x00, -1);

    /* Under MEDIUM ERROR they are an actual retry count. */
    memset(b, 0, sizeof b);
    b[0] = 0x70; b[2] = 0x03; b[7] = 0x0a; b[12] = 0x11; b[13] = 0x00;
    b[15] = 0x80; b[16] = 0x00; b[17] = 0x07;
    check("fixed / MEDIUM ERROR retry count is NOT progress", b, sizeof b, 0, 0x3, 0x11, 0x00, -1);

    memset(b, 0, sizeof b);
    b[0] = 0x70; b[2] = 0x02; b[7] = 0x00; b[12] = 0x04; b[13] = 0x04;
    check("fixed / additional length 0 suppresses ASC/ASCQ", b, sizeof b, 0, 0x2, 0x00, 0x00, -1);

    memset(b, 0, sizeof b);
    b[0] = 0x70;
    check("fixed / below the 8-byte header (len=4)", b, 4, -1, 0, 0, 0, 0);

    /* ---- descriptor format (defensive: DESC=0 should never produce it) ---- */

    memset(b, 0, sizeof b);
    b[0] = 0x72; b[1] = 0x02; b[2] = 0x04; b[3] = 0x04; b[7] = 0x08;
    b[8] = 0x02; b[9] = 0x06; b[12] = 0x80; b[13] = 0x6b; b[14] = 0x84;
    check("descriptor / type 0x02 SKSV=1", b, sizeof b, 0, 0x2, 0x04, 0x04, 0x6b84);

    memset(b, 0, sizeof b);
    b[0] = 0x72; b[1] = 0x02; b[2] = 0x04; b[3] = 0x04; b[7] = 0x08;
    b[8] = 0x02; b[9] = 0x06; b[12] = 0x00; b[13] = 0x6b; b[14] = 0x84;
    check("descriptor / type 0x02 SKSV=0 suppresses progress", b, sizeof b, 0, 0x2, 0x04, 0x04, -1);

    /* Type 0x0A "another progress indication": progress at +6, no SKSV bit. */
    memset(b, 0, sizeof b);
    b[0] = 0x72; b[1] = 0x02; b[2] = 0x04; b[3] = 0x04; b[7] = 0x08;
    b[8] = 0x0a; b[9] = 0x06; b[14] = 0x20; b[15] = 0x00;
    check("descriptor / type 0x0A fallback", b, sizeof b, 0, 0x2, 0x04, 0x04, 0x2000);

    memset(b, 0, sizeof b);
    b[0] = 0x72; b[1] = 0x02; b[2] = 0x04; b[3] = 0x04; b[7] = 0x1c;
    b[8] = 0x00; b[9] = 0x0a;                       /* information descriptor */
    b[20] = 0x02; b[21] = 0x06; b[24] = 0x80; b[25] = 0x30; b[26] = 0x00;
    check("descriptor / SKS after an information descriptor", b, sizeof b, 0, 0x2, 0x04, 0x04, 0x3000);

    memset(b, 0, sizeof b);
    b[0] = 0x72; b[1] = 0x02; b[2] = 0x04; b[3] = 0x04; b[7] = 0x00;
    check("descriptor / no descriptors present", b, sizeof b, 0, 0x2, 0x04, 0x04, -1);

    /* Device-controlled length that overruns the buffer must not be followed. */
    memset(b, 0, sizeof b);
    b[0] = 0x72; b[1] = 0x02; b[2] = 0x04; b[3] = 0x04; b[7] = 0x08;
    b[8] = 0x02; b[9] = 0xff;
    check("descriptor / descriptor lying about its length", b, sizeof b, 0, 0x2, 0x04, 0x04, -1);

    /* ---- malformed ---- */

    memset(b, 0, sizeof b);
    check("unrecognized response code 0x00", b, sizeof b, -1, 0, 0, 0, 0);

    memset(b, 0, sizeof b);
    b[0] = 0xF0;
    check("unrecognized response code 0x70 with valid bit only", b, sizeof b, 0, 0x0, 0x00, 0x00, -1);

    printf("\n%s (%d failure%s)\n", failures ? "FAILURES" : "all tests passed",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
