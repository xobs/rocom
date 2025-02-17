// First, checks if it isn't implemented yet.
if (!String.prototype.format) {
	String.prototype.format = function () {
		var args = arguments;
		return this.replace(/{(\d+)}/g, function (match, number) {
			return typeof args[number] != 'undefined'
				? args[number]
				: match
				;
		});
	};
}

var apList = null;
var selectedSSID = "";
var refreshAPInterval = null;
var checkStatusInterval = null;

function stopCheckStatusInterval() {
	if (checkStatusInterval != null) {
		clearInterval(checkStatusInterval);
		checkStatusInterval = null;
	}
}

function stopRefreshAPInterval() {
	if (refreshAPInterval != null) {
		clearInterval(refreshAPInterval);
		refreshAPInterval = null;
	}
}

function startCheckStatusInterval() {
	checkStatusInterval = setInterval(checkStatus, 950);
}

function startRefreshAPInterval() {
	refreshAPInterval = setInterval(refreshAP, 2800);
}

const slideUp = (selector, callback) => {
	const element = document.querySelector(selector);
	if (element) {
		element.style.transition = "max-height 0.3s ease-out";
		element.style.maxHeight = "0";
		element.addEventListener('transitionend', () => {
			element.style.display = "none";
			if (callback) callback();
		}, { once: true });
	}
};

const slideDown = (selector, callback) => {
	const element = document.querySelector(selector);
	if (element) {
		element.style.display = "block";
		element.style.maxHeight = `${element.scrollHeight}px`;
		if (callback) callback();
	}
};

const setText = (selector, text) => {
	const element = document.querySelector(selector);
	if (element) {
		element.textContent = text;
	}
};

const toggleVisibility = (selector, show) => {
	const element = document.querySelector(selector);
	if (element) {
		element.style.display = show ? "block" : "none";
	}
};


document.addEventListener('DOMContentLoaded', () => {
	const selectedSSID = { value: "" };

	document.querySelector('#wifi-status').addEventListener('click', (event) => {
		if (event.target.classList.contains('ape')) {
			slideUp('#wifi');
			slideDown('#connect-details');
		}
	});

	document.querySelector('#manual_add').addEventListener('click', (event) => {
		if (event.target.classList.contains('ape')) {
			selectedSSID.value = event.target.textContent;
			setText('#ssid-pwd', selectedSSID.value);
			slideUp('#wifi');
			slideDown('#connect_manual');
			slideUp('#connect');
			toggleVisibility('#loading', true);
			toggleVisibility('#connect-success', false);
			toggleVisibility('#connect-fail', false);
		}
	});

	document.querySelector('#wifi-list').addEventListener('click', (event) => {
		if (event.target.classList.contains('ape')) {
			selectedSSID.value = event.target.textContent;
			setText('#ssid-pwd', selectedSSID.value);
			slideUp('#wifi');
			slideUp('#connect_manual');
			slideDown('#connect');
			toggleVisibility('#loading', true);
			toggleVisibility('#connect-success', false);
			toggleVisibility('#connect-fail', false);
		}
	});

	document.querySelector('#cancel').addEventListener('click', () => {
		selectedSSID.value = "";
		slideUp('#connect');
		slideUp('#connect_manual');
		slideDown('#wifi');
	});

	document.querySelector('#manual_cancel').addEventListener('click', () => {
		selectedSSID.value = "";
		slideUp('#connect');
		slideUp('#connect_manual');
		slideDown('#wifi');
	});

	document.querySelector('#join').addEventListener('click', () => {
		performConnect();
	});

	document.querySelector('#manual_join').addEventListener('click', (event) => {
		performConnect(event.target.dataset.connect);
	});

	document.querySelector('#ok-details').addEventListener('click', () => {
		slideUp('#connect-details');
		slideDown('#wifi');
	});

	document.querySelector('#ok-credits').addEventListener('click', () => {
		slideUp('#credits');
		slideDown('#app');
	});

	document.querySelector('#acredits').addEventListener('click', (event) => {
		event.preventDefault();
		slideUp('#app');
		slideDown('#credits');
	});

	document.querySelector('#ok-connect').addEventListener('click', () => {
		slideUp('#connect-wait');
		slideDown('#wifi');
	});

	document.querySelector('#disconnect').addEventListener('click', () => {
		document.querySelector('#connect-details-wrap').classList.add('blur');
		slideDown('#diag-disconnect');
	});

	document.querySelector('#no-disconnect').addEventListener('click', () => {
		slideUp('#diag-disconnect');
		document.querySelector('#connect-details-wrap').classList.remove('blur');
	});

	document.querySelector('#yes-disconnect').addEventListener('click', () => {
		stopCheckStatusInterval();
		selectedSSID.value = "";
		slideUp('#diag-disconnect');
		document.querySelector('#connect-details-wrap').classList.remove('blur');

		fetch('/connect.json', {
			method: 'DELETE',
			headers: { 'Content-Type': 'application/json' },
			body: JSON.stringify({ timestamp: Date.now() })
		});

		startCheckStatusInterval();
		slideUp('#connect-details');
		slideDown('#wifi');
	});

	refreshAP();
	startCheckStatusInterval();
	startRefreshAPInterval();
});



function performConnect(conntype) {
	// Stop the status refresh to prevent race conditions
	stopCheckStatusInterval();

	// Stop refreshing the Wi-Fi list
	stopRefreshAPInterval();

	let pwd;
	if (conntype === 'manual') {
		// Grab the manual SSID and password
		selectedSSID = document.querySelector('#manual_ssid').value;
		pwd = document.querySelector('#manual_pwd').value;
	} else {
		pwd = document.querySelector('#pwd').value;
	}

	// Reset connection status
	toggleVisibility('#loading', true);
	toggleVisibility('#connect-success', false);
	toggleVisibility('#connect-fail', false);

	const okConnectButton = document.querySelector('#ok-connect');
	if (okConnectButton) okConnectButton.disabled = true;

	const ssidWaitElement = document.querySelector('#ssid-wait');
	if (ssidWaitElement) ssidWaitElement.textContent = selectedSSID;

	slideUp('#connect');
	slideUp('#connect_manual');
	slideDown('#connect-wait');

	// Perform the connection request
	fetch('/connect.json', {
		method: 'POST',
		headers: {
			'Content-Type': 'application/json',
			'X-Custom-ssid': selectedSSID,
			'X-Custom-pwd': pwd
		},
		body: JSON.stringify({ timestamp: Date.now() }),
		cache: 'no-cache'
	})
		.then(response => {
			if (!response.ok) {
				throw new Error(`HTTP error! Status: ${response.status}`);
			}
			return response.json();
		})
		.then(data => {
			console.log("Connection response:", data);
		})
		.catch(error => {
			console.error("Error during connection request:", error);
		});

	// Re-set the intervals regardless of the result
	startCheckStatusInterval();
	startRefreshAPInterval();
}



function rssiToIcon(rssi) {
	if (rssi >= -60) {
		return 'w0';
	}
	else if (rssi >= -67) {
		return 'w1';
	}
	else if (rssi >= -75) {
		return 'w2';
	}
	else {
		return 'w3';
	}
}

async function refreshAP() {
	try {
		const response = await fetch("/ap.json");
		if (!response.ok) {
			throw new Error(`HTTP error! Status: ${response.status}`);
		}

		const data = await response.json();

		if (data.length > 0) {
			// Sort by signal strength (rssi) in descending order
			data.sort((a, b) => b.rssi - a.rssi);

			apList = data; // Assuming `apList` is a globally declared variable
			refreshAPHTML(apList);
		}
	} catch (error) {
		console.error("Failed to refresh access points:", error);
	}
}

function refreshAPHTML(data) {
	let htmlContent = data.map((e, idx, array) => {
		const isLast = idx === array.length - 1;
		const brdbClass = isLast ? '' : ' brdb';
		const rssiIconClass = rssiToIcon(e.rssi);
		const passwordClass = e.auth === 0 ? '' : 'pw';
		return `
            <div class="ape${brdbClass}">
                <div class="${rssiIconClass}">
                    <div class="${passwordClass}">${e.ssid}</div>
                </div>
            </div>
        `;
	}).join("\n");

	const wifiListElement = document.querySelector("#wifi-list");
	if (wifiListElement) {
		wifiListElement.innerHTML = htmlContent;
	}
}

async function checkStatus() {
	try {
		const response = await fetch("/status.json");
		if (!response.ok) {
			throw new Error(`HTTP error! Status: ${response.status}`);
		}

		const data = await response.json();

		if (data.hasOwnProperty('ssid') && data['ssid'] !== "") {
			if (data["ssid"] === selectedSSID) {
				// Connection attempt
				if (data["urc"] === 0) {
					// Successful connection
					document.querySelector("#connected-to span").textContent = data["ssid"];
					document.querySelector("#connect-details h1").textContent = data["ssid"];
					document.querySelector("#ip").textContent = data["ip"];
					document.querySelector("#netmask").textContent = data["netmask"];
					document.querySelector("#gw").textContent = data["gw"];
					slideDown("#wifi-status");

					// Unlock the wait screen if needed
					document.querySelector("#ok-connect").disabled = false;

					// Update wait screen
					toggleVisibility("#loading", false);
					toggleVisibility("#connect-success", true);
					toggleVisibility("#connect-fail", false);

					// Redirect to the main page after 5 seconds
					setTimeout(() => {
						window.location.href = `http://${data["ip"]}`;
					}, 5000);
				} else if (data["urc"] === 1) {
					// Failed attempt
					document.querySelector("#connected-to span").textContent = '';
					document.querySelector("#connect-details h1").textContent = '';
					document.querySelector("#ip").textContent = '0.0.0.0';
					document.querySelector("#netmask").textContent = '0.0.0.0';
					document.querySelector("#gw").textContent = '0.0.0.0';

					// Hide connection status
					slideUp("#wifi-status");

					// Unlock the wait screen
					document.querySelector("#ok-connect").disabled = false;

					// Update wait screen
					toggleVisibility("#loading", false);
					toggleVisibility("#connect-fail", true);
					toggleVisibility("#connect-success", false);
				}
			} else if (data.hasOwnProperty('urc') && data['urc'] === 0) {
				// Already connected to Wi-Fi
				if (!document.querySelector("#wifi-status").classList.contains('visible')) {
					document.querySelector("#connected-to span").textContent = data["ssid"];
					document.querySelector("#connect-details h1").textContent = data["ssid"];
					document.querySelector("#ip").textContent = data["ip"];
					document.querySelector("#netmask").textContent = data["netmask"];
					document.querySelector("#gw").textContent = data["gw"];
					slideDown("#wifi-status");
				}
			}
		} else if (data.hasOwnProperty('urc') && data['urc'] === 2) {
			// Manual disconnect
			if (document.querySelector("#wifi-status").classList.contains('visible')) {
				slideUp("#wifi-status");
			}
		}
	} catch (error) {
		// Don't do anything, the server might be down while ESP32 recalibrates radio
		console.error("Failed to fetch status:", error);
	}
}
