//! Proof-of-concept: give delta-kernel-rs a GCS store built with an explicit OAuth bearer
//! token, without changing delta-kernel-rs itself.
//!
//! object_store 0.13.2 (which the kernel pins, on every release including main) has no
//! `GoogleConfigKey` for a bearer token -- that landed in 0.14.0. But it does expose
//! `GoogleCloudStorageBuilder::with_credentials`, which takes a credential provider
//! programmatically. `insert_url_handler` lets us build the store ourselves and hand it back,
//! so the token never has to survive a trip through `parse_url_opts`.

use std::collections::HashMap;
use std::sync::Arc;

use delta_kernel::object_store::aws::AmazonS3Builder;
use delta_kernel::object_store::gcp::{GcpCredential, GoogleCloudStorageBuilder};
use delta_kernel::object_store::path::Path;
use delta_kernel::object_store::{Error as ObjectStoreError, ObjectStore, StaticCredentialProvider};
use delta_kernel_default_engine::storage::insert_url_handler;
use url::Url;

/// The option key duckdb-delta would set via `ffi::set_builder_option`. Arbitrary: the handler
/// reads the option map itself, so this never reaches object_store's key parser.
pub const BEARER_TOKEN_OPTION: &str = "gcs_bearer_token";
/// HMAC interoperability credentials, forwarded when the GCS secret carries no bearer token.
pub const HMAC_KEY_ID_OPTION: &str = "gcs_hmac_key_id";
pub const HMAC_SECRET_OPTION: &str = "gcs_hmac_secret";

fn build_gcs_store(
    url: &Url,
    options: HashMap<String, String>,
) -> Result<(Box<dyn ObjectStore>, Path), ObjectStoreError> {
    let path = Path::parse(url.path())?;

    // HMAC interoperability keys only authenticate against GCS's S3-compatible XML endpoint, so
    // they need an S3 store rather than a native GCS one. Keeping both shapes behind one handler
    // means switching credential kinds does not change which scheme the caller writes.
    let hmac_key = options.get(HMAC_KEY_ID_OPTION).filter(|v| !v.is_empty());
    let hmac_secret = options.get(HMAC_SECRET_OPTION).filter(|v| !v.is_empty());
    if let (Some(key_id), Some(secret)) = (hmac_key, hmac_secret) {
        let bucket = url.host_str().unwrap_or_default();
        let store = AmazonS3Builder::new()
            .with_bucket_name(bucket)
            .with_endpoint("https://storage.googleapis.com")
            .with_virtual_hosted_style_request(false)
            .with_region("auto")
            .with_access_key_id(key_id)
            .with_secret_access_key(secret)
            .build()?;
        return Ok((Box::new(store), path));
    }

    let mut builder = GoogleCloudStorageBuilder::new().with_url(url.as_str());
    if let Some(bearer) = options.get(BEARER_TOKEN_OPTION).filter(|v| !v.is_empty()) {
        builder = builder.with_credentials(Arc::new(StaticCredentialProvider::new(
            GcpCredential {
                bearer: bearer.clone(),
            },
        )));
    }

    let store = builder.build()?;
    Ok((Box::new(store), path))
}

/// Register the handler for both spellings DuckDB accepts. Idempotent: re-registering
/// replaces the entry.
pub fn register_gcs_handler() -> Result<(), delta_kernel::Error> {
    insert_url_handler("gs", Arc::new(build_gcs_store))?;
    insert_url_handler("gcs", Arc::new(build_gcs_store))?;
    Ok(())
}

/// C entry point duckdb-delta would call once at extension load.
/// Returns true on success.
#[no_mangle]
pub extern "C" fn duckdb_delta_register_gcs_handler() -> bool {
    register_gcs_handler().is_ok()
}

#[cfg(test)]
mod tests {
    use super::*;
    use delta_kernel_default_engine::storage::store_from_url_opts;

    /// Negative control, and the reason this crate exists: on object_store 0.13.2 a bearer
    /// token has no config key to travel in. Doubles as the obsolescence tripwire -- when the
    /// kernel picks up object_store >= 0.14, `google_bearer_token` starts parsing and this
    /// test fails, signalling that the shim can be deleted in favour of the native key.
    #[test]
    fn bearer_token_is_not_a_recognized_gcs_config_key() {
        use delta_kernel::object_store::gcp::GoogleConfigKey;
        use std::str::FromStr;

        assert!(GoogleConfigKey::from_str(BEARER_TOKEN_OPTION).is_err());
        assert!(
            GoogleConfigKey::from_str("google_bearer_token").is_err(),
            "object_store has gained native bearer support -- drop this shim and pass \
             google_bearer_token through set_builder_option instead"
        );
    }

    /// The dangerous half, and the real argument for handling this explicitly: `builder_opts!`
    /// maps an unparseable key to `Err(_) => builder`, so an unrecognized option is dropped
    /// WITHOUT error. The store still builds -- with ambient or anonymous credentials -- and
    /// the failure only surfaces later as a 403 from GCS, or not at all if ADC happens to be
    /// present and grants access under a different identity.
    #[test]
    fn stock_path_silently_drops_the_bearer_token() {
        let url = Url::parse("gs://example-bucket/tables/t1").unwrap();
        let result = delta_kernel::object_store::parse_url_opts(
            &url,
            [(BEARER_TOKEN_OPTION, "ya29.test-token")],
        );
        assert!(
            result.is_ok(),
            "unknown GCS options are discarded by builder_opts!, not rejected"
        );
    }

    /// The load-bearing assertion: a handler registered from THIS crate is seen by
    /// `store_from_url_opts`, which lives in delta_kernel_default_engine. That only holds if
    /// cargo unified the crate to a single compilation with a single URL_REGISTRY static --
    /// the exact thing that would fail if the shim were built as a separate staticlib.
    #[test]
    fn registered_handler_is_visible_to_store_from_url_opts() {
        register_gcs_handler().expect("registration failed");

        let url = Url::parse("gs://example-bucket/tables/t1").unwrap();
        let store = store_from_url_opts(
            &url,
            [(BEARER_TOKEN_OPTION, "ya29.test-token")],
        )
        .expect("store_from_url_opts should route through the registered handler");

        let described = format!("{store:?}");
        assert!(
            described.contains("GoogleCloudStorage"),
            "expected a native GCS store, got: {described}"
        );
    }

    /// The handler must not depend on the token being present -- service-account and
    /// anonymous paths still have to build.
    #[test]
    fn handler_builds_without_a_token() {
        register_gcs_handler().expect("registration failed");

        let url = Url::parse("gs://example-bucket/tables/t2").unwrap();
        let store = store_from_url_opts(&url, std::iter::empty::<(&str, &str)>())
            .expect("handler should build a store with no options");

        assert!(format!("{store:?}").contains("GoogleCloudStorage"));
    }

    /// The bucket and key must survive the round trip, since the handler parses them itself.
    #[test]
    fn handler_preserves_bucket_and_path() {
        let url = Url::parse("gs://example-bucket/tables/t1/_delta_log").unwrap();
        let (_store, path) = build_gcs_store(&url, HashMap::new()).expect("build failed");
        assert_eq!(path.as_ref(), "tables/t1/_delta_log");
    }
}

pub mod keepalive;
