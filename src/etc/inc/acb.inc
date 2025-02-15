<?php
/*
 * autoconfigbackup.inc
 *
 * part of pfSense (https://www.pfsense.org)
 * Copyright (c) 2008-2013 BSD Perimeter
 * Copyright (c) 2013-2016 Electric Sheep Fencing
 * Copyright (c) 2014-2019 Rubicon Communications, LLC (Netgate)
 * All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

require_once("filter.inc");
require_once("notices.inc");

// If there is no ssh key in the system to identify this firewall, generate a pair now
function get_device_key() {
	if (!file_exists("/etc/ssh/ssh_host_ed25519_key.pub")) {
		exec("/usr/bin/nice -n20 /usr/bin/ssh-keygen -t ed25519 -b 4096 -N '' -f /etc/ssh//ssh_host_ed25519_key");
	}

	// The userkey (firewall identifier) is an MD5 has of the ed25519 public key
	return hash("sha256", file_get_contents("/etc/ssh/ssh_host_ed25519_key.pub"));
}

$origkey = get_device_key();

if (isset($_REQUEST['userkey']) && !empty($_REQUEST['userkey'])) {
	$userkey = htmlentities($_REQUEST['userkey']);
} else {
	$userkey = get_device_key();
}

// Defined username. Username must be sent lowercase. See Redmine #7127 and Netgate Redmine #163
$username = strtolower($config['system']['acb']['gold_username']);

// Defined password
$password = $config['system']['acb']['gold_password'];

$uniqueID = system_get_uniqueid();

/* Check whether ACB is enabled */
function acb_enabled() {
	global $config;
	$acb_enabled = false;

	if (is_array($config['system']['acb'])) {
		if ($config['system']['acb']['enable'] == "yes") {
			$acb_enabled = true;
		}
	}

	return $acb_enabled;
}

// Ensures patches match
function acb_custom_php_validation_command($post, &$input_errors) {
	global $_POST, $savemsg, $config;

	// Do nothing when ACB is disabled in configuration
	// This also makes it possible to delete the credentials from config.xml
	if (!acb_enabled()) {
		// We do not need to store this value.
		unset($_POST['testconnection']);
		return;
	}

	if (!$post['crypto_password'] or !$post['crypto_password2']) {
		$input_errors[] = "The encryption password is required.";
	}

	if ($post['crypto_password'] <> $post['crypto_password2']) {
		$input_errors[] = "Sorry, the entered encryption passwords do not match.";
	}

	if ($post['testconnection']) {
		$status = test_connection($post);
		if ($status) {
			$savemsg = "Connection to the ACB server was tested with no errors.";
		}
	}

	// We do not need to store this value.
	unset($_POST['testconnection']);
}

function acb_custom_php_resync_config_command() {
	// Do nothing when ACB is disabled in configuration
	if (!acb_enabled()) {
		return;
	}

	unlink_if_exists("/cf/conf/lastpfSbackup.txt");

	if (!function_exists("filter_configure")) {
		require_once("filter.inc");
	}

	filter_configure();

	if ($savemsg) {
		$savemsg .= "<br/>";
	}

	$savemsg .= "A configuration backup has been queued.";
}

function configure_proxy() {
	global $config;
	$ret = array();
	if (!empty($config['system']['proxyurl'])) {
		$ret[CURLOPT_PROXY] = $config['system']['proxyurl'];
		if (!empty($config['system']['proxyport'])) {
			$ret[CURLOPT_PROXYPORT] = $config['system']['proxyport'];
		}
		if (!empty($config['system']['proxyuser']) && !empty($config['system']['proxypass'])) {
			$ret[CURLOPT_PROXYAUTH] = CURLAUTH_ANY | CURLAUTH_ANYSAFE;
			$ret[CURLOPT_PROXYUSERPWD] = "{$config['system']['proxyuser']}:{$config['system']['proxypass']}";
		}
	}
	return $ret;
}

function test_connection($post) {
	global $savemsg, $config, $g, $legacy;

	// Do nothing when booting or when not enabled
	if (platform_booting() || !acb_enabled()) {
		return;
	}

	// Seperator used during client / server communications
	$oper_sep = "\|\|";

	// Encryption password
	$decrypt_password = $post['crypto_password'];

	// Defined username. Username must be sent lowercase. See Redmine #7127 and Netgate Redmine #163
	$username = strtolower($post['username']);

	// Defined password
	$password = $post['password'];

	// Set hostname
	$hostname = $config['system']['hostname'] . "." . $config['system']['domain'];

	// URL to restore.php
	$get_url = $legacy ? "https://portal.pfsense.org/pfSconfigbackups/restore.php":"https://acb.netgate.com/getbkp";

	// Populate available backups
	$curl_session = curl_init();
	curl_setopt($curl_session, CURLOPT_URL, $get_url);

	if ($legacy) {
		curl_setopt($curl_session, CURLOPT_HTTPHEADER, array("Authorization: Basic " . base64_encode("{$username}:{$password}")));
	}

	curl_setopt($curl_session, CURLOPT_SSL_VERIFYPEER, 1);
	curl_setopt($curl_session, CURLOPT_POST, 1);
	curl_setopt($curl_session, CURLOPT_RETURNTRANSFER, 1);
	curl_setopt($curl_session, CURLOPT_CONNECTTIMEOUT, 55);
	curl_setopt($curl_session, CURLOPT_TIMEOUT, 30);
	curl_setopt($curl_session, CURLOPT_USERAGENT, $g['product_name'] . '/' . rtrim(file_get_contents("/etc/version")));
	// Proxy
	curl_setopt_array($curl_session, configure_proxy());

	curl_setopt($curl_session, CURLOPT_POSTFIELDS, "action=showbackups&hostname={$hostname}");
	$data = curl_exec($curl_session);

	if (curl_errno($curl_session)) {
		return("An error occurred " . curl_error($curl_session));
	} else {
		curl_close($curl_session);
	}

	return;
}

function upload_config($reasonm = "") {
	global $config, $g, $input_errors, $userkey, $uniqueID;

	if (empty($userkey)) {
		$userkey = get_device_key();
	}

	if (empty($uniqueID)) {
		$uniqueID = system_get_uniqueid();
	}

	// Do nothing when booting or when not enabled
	if (platform_booting() || !acb_enabled()) {
		return;
	}

	/*
	 * pfSense upload config to pfSense.org script
	 * This file plugs into config.inc (/usr/local/pkg/parse_config)
	 * and runs every time the running firewall filter changes.
	 *
	 */

	if (file_exists("/tmp/acb_nooverwrite")) {
		unlink("/tmp/acb_nooverwrite");
		$nooverwrite = "true";
	} else {
		$nooverwrite = "false";
	}

	// Define some needed variables
	if (file_exists("/cf/conf/lastpfSbackup.txt")) {
		$last_backup_date = str_replace("\n", "", file_get_contents("/cf/conf/lastpfSbackup.txt"));
	} else {
		$last_backup_date = "";
	}

	$last_config_change = $config['revision']['time'];
	$hostname = $config['system']['hostname'] . "." . $config['system']['domain'];

	if ($reasonm) {
		$reason = $reasonm;
	} else {
		$reason	= $config['revision']['description'];
	}

	/*
	* Syncing vouchers happens every minute and sometimes multiple times. We don't
	* want to fill up our db with a lot of the same config so just ignore that case.
	*/
	if((strpos($reason, 'Syncing vouchers') !== false ||
		strpos($reason, 'Captive Portal Voucher database synchronized') !== false) ) {
		return;
	}


	$encryptpw = $config['system']['acb']['encryption_password'];

	// Define upload_url, must be present after other variable definitions due to username, password
	$upload_url = "https://acb.netgate.com/save";

	if (!$encryptpw) {
		if (!file_exists("/cf/conf/autoconfigback.notice")) {
			$notice_text = "The encryption password is not set for Automatic Configuration Backup.";
			$notice_text .= " Please correct this in Services -> AutoConfigBackup -> Settings.";
			//log_error($notice_text);
			//file_notice("AutoConfigBackup", $notice_text, $notice_text, "");
			touch("/cf/conf/autoconfigback.notice");
		}
	} else {
		/* If configuration has changed, upload to pfS */
		if ($last_backup_date <> $last_config_change) {

			$notice_text = "Beginning configuration backup to ." . $upload_url;
			log_error($notice_text);
			update_filter_reload_status($notice_text);

			// Encrypt config.xml
			$data = file_get_contents("/cf/conf/config.xml");
			$raw_config_sha256_hash = trim(shell_exec("/sbin/sha256 /cf/conf/config.xml | /usr/bin/awk '{ print $4 }'"));
			$data = encrypt_data($data, $encryptpw);
			$tmpname = "/tmp/" . $raw_config_sha256_hash . ".tmp";
			tagfile_reformat($data, $data, "config.xml");
			file_put_contents($tmpname, $data);

			$post_fields = array(
				'reason' => htmlspecialchars($reason),
				'uid' => $uniqueID,
				'file' => curl_file_create($tmpname, 'image/jpg', 'config.jpg'),
				'userkey' => htmlspecialchars($userkey),
				'sha256_hash' => $raw_config_sha256_hash,
				'version' => $g['product_version'],
				'hint' => $config['system']['acb']['hint']
			);

			// Check configuration into the ESF repo
			$curl_session = curl_init();

			curl_setopt($curl_session, CURLOPT_URL, "https://acb.netgate.com/save");
			curl_setopt($curl_session, CURLOPT_POST, count($post_fields));
			curl_setopt($curl_session, CURLOPT_POSTFIELDS, $post_fields);
			curl_setopt($curl_session, CURLOPT_RETURNTRANSFER, 1);
			curl_setopt($curl_session, CURLOPT_SSL_VERIFYPEER, 0);
			curl_setopt($curl_session, CURLOPT_CONNECTTIMEOUT, 55);
			curl_setopt($curl_session, CURLOPT_TIMEOUT, 30);
			curl_setopt($curl_session, CURLOPT_USERAGENT, $g['product_name'] . '/' . rtrim(file_get_contents("/etc/version")));
			// Proxy
			curl_setopt_array($curl_session, configure_proxy());

			$data = curl_exec($curl_session);

			unlink_if_exists($tmpname);

			if (curl_errno($curl_session)) {
				$fd = fopen("/tmp/backupdebug.txt", "w");
				fwrite($fd, $upload_url . "" . $fields_string . "\n\n");
				fwrite($fd, $data);
				fwrite($fd, curl_error($curl_session));
				fclose($fd);
			} else {
				curl_close($curl_session);
			}

			if (strpos($data, "500") != false) {
				$notice_text = "An error occurred while uploading your pfSense configuration to " . $upload_url . " (" . htmlspecialchars($data) . ")";
				log_error($notice_text . " - " . $data);
				file_notice("AutoConfigBackup", $notice_text, "Backup Failure", "/pkg_edit.php?xml=autoconfigbackup.xml&id=0");
				update_filter_reload_status($notice_text);
				$input_errors["acb_upload"] = $notice_text;
			} else {
				// Update last pfS backup time
				$fd = fopen("/cf/conf/lastpfSbackup.txt", "w");
				fwrite($fd, $config['revision']['time']);
				fclose($fd);
				$notice_text = "End of configuration backup to " . $upload_url . " (success).";
				log_error($notice_text);
				update_filter_reload_status($notice_text);
			}

		} else {
			// Debugging
			//log_error("No https://portal.pfsense.org backup required.");
		}
	}
}
