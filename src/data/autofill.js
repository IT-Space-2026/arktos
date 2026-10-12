// Armed per navigation by AutoFillManager::scheduleOnPage() as a
// per-page QWebEngineScript at DocumentReady in the page's main
// world.  Fills stored form data immediately, and — when CAPTURE_FLAG
// is true — reports submitted forms back through the "aroraAutofill"
// QWebChannel object.  FORMS_JSON, CAPTURE_FLAG and REPORT_TOKEN are
// substituted C++-side; all three stay inside this closure, so page
// script cannot read the token that authenticates a report (SEC08).
//
// The client is opened lazily and shared: qt.webChannelTransport has
// a single onmessage slot per frame and every `new QWebChannel`
// replaces it, so a second client steals the first client's in-flight
// responses — the recurring "execCallbacks[message.id] is not a
// function" errors (CHAN01) came from an autofill client colliding
// with page-side channel users such as the start page.  WebPage arms
// arora-channel.js at DocumentCreation, which owns the transport's
// dispatch and hands out one shared client via __aroraChannel(); the
// fallback below keeps the attachToPage path working on pages that
// never ran the bootstrap.
(function () {
    var savedForms = FORMS_JSON;
    var savedAddress = ADDRESS_JSON;
    var capture = CAPTURE_FLAG;
    var reportToken = "REPORT_TOKEN";

    var pendingReports = [];
    var bridge = null;
    var connecting = false;

    // ADDR02: maps a form field to an Address member.  The html
    // autocomplete attribute is authoritative — its field token is
    // the LAST token (section-*/shipping/billing modifiers precede
    // it); name/id heuristics only run when no token resolves.
    var TOKEN_MEMBER = {
        'name': 'name',
        'organization': 'organization',
        'street-address': 'street',
        'address-line1': 'street1',
        'address-line2': 'street2',
        'address-line3': 'street3',
        'address-level2': 'city',
        'address-level1': 'state',
        'postal-code': 'zip',
        'tel': 'phone',
        'tel-national': 'phone',
        'email': 'email',
        'country': 'country',
        'country-name': 'country'
    };
    var NAME_MEMBER = {
        'name': 'name', 'fullname': 'name', 'yourname': 'name',
        'customername': 'name', 'contactname': 'name',
        'fname': 'nameFirst', 'firstname': 'nameFirst',
        'first': 'nameFirst', 'givenname': 'nameFirst',
        'lname': 'nameLast', 'lastname': 'nameLast',
        'last': 'nameLast', 'surname': 'nameLast',
        'familyname': 'nameLast',
        'organization': 'organization', 'organisation': 'organization',
        'company': 'organization', 'companyname': 'organization',
        'business': 'organization', 'employer': 'organization',
        'address': 'street1', 'address1': 'street1', 'addr': 'street1',
        'addr1': 'street1', 'street': 'street1', 'street1': 'street1',
        'streetaddress': 'street1', 'addressline': 'street1',
        'addressline1': 'street1', 'billingaddress': 'street1',
        'billingaddress1': 'street1', 'shipaddress': 'street1',
        'shippingaddress': 'street1', 'shippingaddress1': 'street1',
        'address2': 'street2', 'addr2': 'street2', 'street2': 'street2',
        'addressline2': 'street2', 'billingaddress2': 'street2',
        'shippingaddress2': 'street2', 'shipaddress2': 'street2',
        'apartment': 'street2', 'apt': 'street2', 'suite': 'street2',
        'unit': 'street2',
        'address3': 'street3', 'addr3': 'street3', 'street3': 'street3',
        'addressline3': 'street3',
        'city': 'city', 'town': 'city', 'locality': 'city',
        'billingcity': 'city', 'shippingcity': 'city',
        'state': 'state', 'province': 'state', 'region': 'state',
        'county': 'state', 'billingstate': 'state',
        'shippingstate': 'state',
        'zip': 'zip', 'zipcode': 'zip', 'postalcode': 'zip',
        'postcode': 'zip', 'postal': 'zip', 'billingzip': 'zip',
        'billingpostalcode': 'zip', 'shippingzip': 'zip',
        'country': 'country', 'countryname': 'country',
        'countrycode': 'country',
        'phone': 'phone', 'tel': 'phone', 'telephone': 'phone',
        'phonenumber': 'phone', 'mobile': 'phone', 'cell': 'phone',
        'cellphone': 'phone', 'billingphone': 'phone',
        'shippingphone': 'phone',
        'email': 'email', 'mail': 'email', 'emailaddress': 'email',
        'emailid': 'email', 'billingemail': 'email'
    };

    function autocompleteTokens(el) {
        var raw = (el.getAttribute('autocomplete') || '').toLowerCase();
        return raw.split(/\s+/).filter(function (t) { return t; });
    }

    function isAutocompleteOff(el) {
        return autocompleteTokens(el).indexOf('off') >= 0;
    }

    function memberForElement(el) {
        var tokens = autocompleteTokens(el);
        if (tokens.length) {
            var member = TOKEN_MEMBER[tokens[tokens.length - 1]];
            if (member)
                return member;
        }
        var key = (el.name || '')
            .toLowerCase().replace(/[^a-z0-9]/g, '');
        member = NAME_MEMBER[key];
        if (member)
            return member;
        key = (el.id || '')
            .toLowerCase().replace(/[^a-z0-9]/g, '');
        return NAME_MEMBER[key] || null;
    }

    function isFillableField(el) {
        if (!el || el.disabled || el.readOnly)
            return false;
        var tag = el.tagName;
        if (tag == 'SELECT' || tag == 'TEXTAREA')
            return true;
        if (tag != 'INPUT')
            return false;
        var type = (el.type || 'text').toLowerCase();
        return ['text', 'tel', 'email', 'url', 'search',
                'number', ''].indexOf(type) >= 0;
    }

    function memberValue(member) {
        var a = savedAddress;
        switch (member) {
        case 'name': return a.name;
        case 'nameFirst': return a.name.split(/\s+/)[0] || '';
        case 'nameLast': return a.name.split(/\s+/).slice(1).join(' ');
        case 'organization': return a.organization;
        case 'street': return a.street;
        case 'street1': return a.street.split('\n')[0] || '';
        case 'street2': return a.street.split('\n')[1] || '';
        case 'street3': return a.street.split('\n')[2] || '';
        case 'city': return a.city;
        case 'state': return a.state;
        case 'zip': return a.zip;
        case 'phone': return a.phone;
        case 'email': return a.email;
        case 'country': return a.country;
        }
        return '';
    }

    // A <select> fills when an option's value or text matches — for
    // country the stored ISO code usually matches the option value.
    function selectValue(el, value) {
        var needle = value.toLowerCase();
        for (var i = 0; i < el.options.length; ++i) {
            var option = el.options[i];
            if (option.value.toLowerCase() == needle
                || option.text.toLowerCase() == needle) {
                el.value = option.value;
                return true;
            }
        }
        return false;
    }

    // The single address the store picked for this page fills
    // whichever fields it maps onto — same trigger (DocumentReady)
    // and overwrite semantics as the stored-form fill.  A form that
    // received a fill is tagged so its submit report can mark the
    // address used back in the manager.
    function fillAddress() {
        if (!savedAddress)
            return;
        for (var f = 0; f < document.forms.length; ++f) {
            var form = document.forms[f];
            var filled = false;
            for (var i = 0; i < form.elements.length; ++i) {
                var el = form.elements[i];
                if (!isFillableField(el))
                    continue;
                var member = memberForElement(el);
                if (!member)
                    continue;
                var value = memberValue(member);
                if (!value)
                    continue;
                if (el.tagName == 'SELECT') {
                    if (!selectValue(el, value))
                        continue;
                } else {
                    el.value = value;
                }
                filled = true;
            }
            if (filled)
                form.__aroraAddrId = savedAddress.id;
        }
    }

    function fillForms() {
        for (var i = 0; i < savedForms.length; ++i) {
            var saved = savedForms[i];
            // Same lookup as the Qt4 code: document.forms["name"],
            // falling back to the first form for unnamed entries.
            var form = saved.name ? document.forms[saved.name]
                                  : document.forms[0];
            if (!form || form.tagName != 'FORM')
                continue;
            for (var j = 0; j < saved.elements.length; ++j) {
                var pair = saved.elements[j];
                var el = form.elements[pair.name];
                if (!el || el.disabled || el.readOnly)
                    continue;
                var type = (el.type || '').toLowerCase();
                if (!type || type == 'hidden' || type == 'reset'
                    || type == 'submit')
                    continue;
                if (type == 'checkbox')
                    el.checked = pair.value != '' && pair.value != 'false';
                else
                    el.value = pair.value;
            }
        }
    }

    function serializeForm(form) {
        var data = { name: form.name || '', hasPassword: false,
                     elements: [], addressFields: [] };
        for (var i = 0; i < form.elements.length; ++i) {
            var el = form.elements[i];
            if (!el.name || el.disabled)
                continue;
            var type = (el.type || '').toLowerCase();
            if ((type == 'checkbox' || type == 'radio') && !el.checked)
                continue;
            if (type == 'password')
                data.hasPassword = true;
            data.elements.push({
                name: el.name,
                value: el.value,
                autocomplete: el.getAttribute('autocomplete') || ''
            });
            // ADDR02: every mapped address field rides the report so
            // the manager can build the save candidate — including
            // the per-element autocomplete="off" flag it filters on.
            if (isFillableField(el)) {
                var member = memberForElement(el);
                if (member && el.value && el.value.trim()) {
                    data.addressFields.push({
                        member: member,
                        value: el.value,
                        off: isAutocompleteOff(el)
                    });
                }
            }
        }
        // A form our fill touched reports which stored address it
        // used — the MRU stamp back in the manager.
        if (savedAddress && form.__aroraAddrId == savedAddress.id)
            data.usedAddressId = savedAddress.id;
        // Only forms carrying a password are ever stored (matches the
        // old passwordForms setting); address-shaped submits still
        // ride the channel so the save-address prompt can fire.
        var keep = (data.hasPassword && data.elements.length)
            || data.addressFields.length;
        return keep ? data : null;
    }

    function flushReports() {
        while (bridge && pendingReports.length)
            bridge.submitForm(reportToken, String(location.href),
                              pendingReports.shift());
    }

    function connectChannel() {
        // __aroraChannel (arora-channel.js) serves the one shared
        // client; it queues this callback if the handshake is still
        // in flight, so no second client ever competes for the
        // transport's onmessage slot.
        if (window.__aroraChannel) {
            window.__aroraChannel(function (channel) {
                bridge = channel.objects.aroraAutofill || null;
                flushReports();
            });
            return;
        }
        // Fallback for documents where the DocumentCreation bootstrap
        // was never armed (attachToPage on a bare page).
        if (!window.qt || !qt.webChannelTransport || !window.QWebChannel)
            return;
        connecting = true;
        new QWebChannel(qt.webChannelTransport, function (channel) {
            connecting = false;
            bridge = channel.objects.aroraAutofill || null;
            flushReports();
        });
    }

    function installCapture() {
        var report = function (form) {
            if (!form || form.tagName != 'FORM')
                return;
            var data = serializeForm(form);
            if (data) {
                pendingReports.push(data);
                if (!bridge)
                    connectChannel();
            }
        };
        document.addEventListener('submit', function (event) {
            report(event.target);
        }, true);
        // form.submit() bypasses the submit event entirely; wrap it so
        // scripted submissions are captured too.
        var nativeSubmit = HTMLFormElement.prototype.submit;
        HTMLFormElement.prototype.submit = function () {
            report(this);
            return nativeSubmit.call(this);
        };
    }

    function start() {
        fillForms();
        fillAddress();
        if (capture)
            installCapture();
    }

    if (window.QWebChannel || !capture) {
        // qwebchannel.js was embedded into this script C++-side when
        // the resource was reachable; without capture there is no need
        // for the channel at all.
        start();
        return;
    }
    // Fallback: pull the channel client from the builtin qrc scheme.
    var loader = document.createElement('script');
    loader.src = 'qrc:///qtwebchannel/qwebchannel.js';
    loader.onload = start;
    (document.head || document.documentElement).appendChild(loader);
}())
