#include <stir_shaken.h>


stir_shaken_status_t stir_shaken_unit_test_sign_verify_data(void)
{
	stir_shaken_status_t status = STIR_SHAKEN_STATUS_FALSE;
	const char *data_test_pass = "unit test 2 pass";
	const char *data_test_fail = "unit test 2 fail";
	size_t datalen = 0;
	unsigned char sig[PBUF_LEN] = { 0 };
	size_t outlen = 0;

	char private_key_name[300] = { 0 };
	char public_key_name[300] = { 0 };

	EC_KEY *ec_key = NULL;
	EVP_PKEY *private_key = NULL;
	EVP_PKEY *public_key = NULL;
	int i = -1;

	pthread_mutex_lock(&stir_shaken_globals.mutex);
	sprintf(private_key_name, "%s%c%s", stir_shaken_globals.settings.path, '/', "u1_private_key.pem");
	sprintf(public_key_name, "%s%c%s", stir_shaken_globals.settings.path, '/', "u1_public_key.pem");
	pthread_mutex_unlock(&stir_shaken_globals.mutex);

	printf("=== Unit testing: STIR/Shaken Sign-verify (in memory) [stir_shaken_unit_test_sign_verify_data]\n\n");

	// Generate new keys for this test
	pthread_mutex_lock(&stir_shaken_globals.mutex);
	status = stir_shaken_generate_keys(&ec_key, &private_key, &public_key, private_key_name, public_key_name);
	pthread_mutex_unlock(&stir_shaken_globals.mutex);

	stir_shaken_assert(status == STIR_SHAKEN_STATUS_OK, "Err, failed to generate keys...");
	stir_shaken_assert(ec_key != NULL, "Err, failed to generate EC key");
	stir_shaken_assert(private_key != NULL, "Err, failed to generate private key");
	stir_shaken_assert(public_key != NULL, "Err, failed to generate public key");

	/* Test */
	printf("Signing...\n\n");

	datalen = strlen(data_test_pass);
	outlen = sizeof(sig);
	status = stir_shaken_do_sign_data_with_digest("sha256", private_key, data_test_pass, datalen, sig, &outlen);
	stir_shaken_assert(status == STIR_SHAKEN_STATUS_OK, "Failed to sign\n");

	printf("Verifying (against good data)...\n\n");
	i = stir_shaken_do_verify_data(data_test_pass, strlen(data_test_pass), sig, outlen, public_key);
	stir_shaken_assert(i == 0, "Err, verify failed");

	printf("Verifying (against bad data)...\n\n");
	i = stir_shaken_do_verify_data(data_test_fail, strlen(data_test_fail), sig, outlen, public_key);
	stir_shaken_assert(i == 1, "Err, verify failed");
	
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

	if (stir_shaken_unit_test_sign_verify_data() != STIR_SHAKEN_STATUS_OK) {
		
		printf("Fail\n");
		return -2;
	}

	printf("OK\n");

	// For testing purposes need to call SSL deinit from here, otherwise valgrind doesn't catch released memory that is returned by a call to stir_shaken_deinit made by library deallocator	
	stir_shaken_do_deinit();

	return 0;
}
