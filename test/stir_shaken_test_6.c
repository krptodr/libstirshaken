#include <stir_shaken.h>


stir_shaken_status_t stir_shaken_unit_test_authorize(void)
{
    const char *x5u = "https://not.here.org/passport.cer";
    const char *attest = "B";
    const char *desttn_key = "uri";
    const char *desttn_val = "sip:Obama@democrats.com";
    int iat = 9876543;
    const char *origtn_key = "";
    const char *origtn_val = "07483866525";
    const char *origid = "Trump's Office";
    char *sih = NULL;
    stir_shaken_status_t status = STIR_SHAKEN_STATUS_FALSE;

    stir_shaken_passport_params_t params = { .x5u = x5u, .attest = attest, .desttn_key = desttn_key, .desttn_val = desttn_val, .iat = iat, .origtn_key = origtn_key, .origtn_val = origtn_val, .origid = origid };

	char private_key_name[300] = { 0 };
	char public_key_name[300] = { 0 };
    
    EC_KEY *ec_key = NULL;
    EVP_PKEY *private_key = NULL;
    EVP_PKEY *public_key = NULL;

	pthread_mutex_lock(&stir_shaken_globals.mutex);
	sprintf(private_key_name, "%s%c%s", stir_shaken_globals.settings.path, '/', "u6_private_key.pem");
	sprintf(public_key_name, "%s%c%s", stir_shaken_globals.settings.path, '/', "u6_public_key.pem");
	pthread_mutex_unlock(&stir_shaken_globals.mutex);

    printf("=== Unit testing: STIR/Shaken Authorization [stir_shaken_unit_test_authorize]\n\n");
    
    // Generate new keys for this test
    status = stir_shaken_generate_keys(&ec_key, &private_key, &public_key, private_key_name, public_key_name);
    stir_shaken_assert(status == STIR_SHAKEN_STATUS_OK, "Err, failed to generate keys...");
    stir_shaken_assert(ec_key != NULL, "Err, failed to generate EC key\n\n");
    stir_shaken_assert(private_key != NULL, "Err, failed to generate private key");
    stir_shaken_assert(public_key != NULL, "Err, failed to generate public key");

    /* Test */
    status = stir_shaken_authorize(&sih, &params, private_key, NULL);
    stir_shaken_assert(status == STIR_SHAKEN_STATUS_OK, "Failed to create SIP Identity Header");
    stir_shaken_assert(sih != NULL, "Failed to create SIP Identity Header");
    
    printf("OK\n\n");
	
	free(sih);
	sih = NULL;
    
	pthread_mutex_lock(&stir_shaken_globals.mutex);
	stir_shaken_destroy_keys(&ec_key, &private_key, &public_key);
	pthread_mutex_unlock(&stir_shaken_globals.mutex);
    
    return STIR_SHAKEN_STATUS_OK;
}

int main(void)
{
	const char *path = "./test/run";

	if (stir_shaken_dir_exists(path) != STIR_SHAKEN_STATUS_OK) {

		if (stir_shaken_dir_create_recursive(path) != STIR_SHAKEN_STATUS_OK) {
	
			printf("ERR: Cannot create test dir\n");
			return -1;
		}
	}

	stir_shaken_settings_set_path(path);

	if (stir_shaken_unit_test_authorize() != STIR_SHAKEN_STATUS_OK) {
		
		printf("Fail\n");
		return -2;
	}

	printf("OK\n");

	return 0;
}
