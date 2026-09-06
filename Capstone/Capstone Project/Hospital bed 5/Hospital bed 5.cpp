
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "Ws2_32.lib")

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <cctype>

using namespace std;

// ============================ UTILITIES ==============================

string nowString() {
    time_t t = time(nullptr);
    tm localTm{};
    localtime_s(&localTm, &t);

    stringstream ss;
    ss << put_time(&localTm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

string lowerCopy(string s) {
    transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) {
            return static_cast<char>(tolower(c));
        });
    return s;
}

string trim(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

string jsonEscape(const string& s) {
    string out;

    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }

    return out;
}

string urlDecode(const string& in) {
    string out;

    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') {
            out += ' ';
        }
        else if (in[i] == '%' && i + 2 < in.size()) {
            string hex = in.substr(i + 1, 2);
            char ch = static_cast<char>(
                strtol(hex.c_str(), nullptr, 16)
                );
            out += ch;
            i += 2;
        }
        else {
            out += in[i];
        }
    }

    return out;
}

map<string, string> parseForm(const string& body) {
    map<string, string> result;
    stringstream ss(body);
    string part;

    while (getline(ss, part, '&')) {
        size_t eq = part.find('=');

        if (eq == string::npos)
            continue;

        string key = urlDecode(part.substr(0, eq));
        string value = urlDecode(part.substr(eq + 1));

        result[key] = value;
    }

    return result;
}

int toInt(const string& s, int fallback = 0) {
    try {
        return stoi(s);
    }
    catch (...) {
        return fallback;
    }
}

double toDouble(const string& s, double fallback = 0.0) {
    try {
        return stod(s);
    }
    catch (...) {
        return fallback;
    }
}

string normalizePriority(string priority) {
    priority = lowerCopy(trim(priority));

    if (priority == "critical") return "CRITICAL";
    if (priority == "high") return "HIGH";
    if (priority == "medium") return "MEDIUM";
    if (priority == "low") return "LOW";

    return "";
}

// ============================== PATIENT ===============================

class Patient {
public:
    int id = 0;

    string name;
    int age = 0;
    string gender;
    string phone;

    // Clinical details - STORED ONLY.
    double spo2 = 0;
    int heartRate = 0;
    string complaint;

    // Condition directly determines priority.
    string condition;
    string priority;

    string bedId;
    string status = "Waiting";
    string registeredAt;
};

// ================================ BED =================================

class Bed {
public:
    string id;
    string ward;
    string type;

    // Available / Occupied / Reserved / Maintenance
    string status = "Available";
    int patientId = 0;
};

// ============================== RESOURCE ==============================

class Resource {
public:
    int id = 0;
    string name;
    string category;
    int total = 0;
    int available = 0;
};

// ========================== HOSPITAL SYSTEM ===========================

class HospitalSystem {
private:
    vector<Patient> patients;
    vector<Bed> beds;
    vector<Resource> resources;

    mutex mtx;

    int nextPatientId = 1001;
    int nextResourceId = 1;

    const string PATIENT_FILE = "patients.txt";
    const string BED_FILE = "beds.txt";
    const string RESOURCE_FILE = "resources.txt";
    const string AUDIT_FILE = "audit.log";

    void auditUnlocked(const string& action) {
        ofstream out(AUDIT_FILE, ios::app);

        if (out)
            out << nowString() << " | " << action << "\n";
    }

    void initializeBedsUnlocked() {
        // Always make sure exactly 50 beds exist if beds.txt is missing.
        if (beds.size() >= 50)
            return;

        beds.clear();

        // 10 ICU
        for (int i = 1; i <= 10; ++i)
            beds.push_back({
                "ICU-" + to_string(i),
                "ICU",
                "ICU",
                "Available",
                0
                });

        // 15 Emergency
        for (int i = 1; i <= 15; ++i)
            beds.push_back({
                "ER-" + to_string(i),
                "Emergency",
                "Emergency",
                "Available",
                0
                });

        // 25 General
        for (int i = 1; i <= 25; ++i)
            beds.push_back({
                "GEN-" + to_string(i),
                "General",
                "General",
                "Available",
                0
                });

        // Restore occupancy from active patients.
        for (auto& p : patients) {
            if (p.status == "Discharged" || p.bedId.empty())
                continue;

            for (auto& b : beds) {
                if (b.id == p.bedId) {
                    b.status = "Occupied";
                    b.patientId = p.id;
                    break;
                }
            }
        }
    }

    void initializeResourcesUnlocked() {
        if (!resources.empty())
            return;

        resources.push_back({ nextResourceId++, "Ventilator", "Critical Care", 8, 8 });
        resources.push_back({ nextResourceId++, "Cardiac Monitor", "Monitoring", 15, 15 });
        resources.push_back({ nextResourceId++, "Infusion Pump", "Treatment", 20, 20 });
        resources.push_back({ nextResourceId++, "Oxygen Cylinder", "Emergency", 25, 25 });
        resources.push_back({ nextResourceId++, "Wheelchair", "Mobility", 12, 12 });
    }

public:

    HospitalSystem() {
        lock_guard<mutex> lock(mtx);

        loadAllUnlocked();
        initializeBedsUnlocked();
        initializeResourcesUnlocked();

        savePatientsUnlocked();
        saveBedsUnlocked();
        saveResourcesUnlocked();
    }

    // ========================== FILE LOAD =============================

    void loadAllUnlocked() {

        patients.clear();
        beds.clear();
        resources.clear();

        /*
          Patient file format:
          id|name|age|gender|phone|spo2|heartRate|complaint|condition|priority|bed|status|registeredAt

          The code also attempts compatibility with the previous format.
        */

        {
            ifstream in(PATIENT_FILE);
            string line;

            while (getline(in, line)) {

                if (line.empty())
                    continue;

                vector<string> f;
                string x;
                stringstream ss(line);

                while (getline(ss, x, '|'))
                    f.push_back(x);

                // New format = 13 fields.
                if (f.size() >= 13) {

                    Patient p;

                    p.id = toInt(f[0]);
                    p.name = f[1];
                    p.age = toInt(f[2]);
                    p.gender = f[3];
                    p.phone = f[4];
                    p.spo2 = toDouble(f[5]);
                    p.heartRate = toInt(f[6]);
                    p.complaint = f[7];

                    p.condition = normalizePriority(f[8]);
                    if (p.condition.empty())
                        p.condition = "LOW";

                    p.priority = p.condition;
                    p.bedId = f[10];
                    p.status = f[11];
                    p.registeredAt = f[12];

                    patients.push_back(p);

                    nextPatientId =
                        max(nextPatientId, p.id + 1);
                }
                // Older 10-field format:
                else if (f.size() >= 10) {

                    Patient p;

                    p.id = toInt(f[0]);
                    p.name = f[1];
                    p.age = toInt(f[2]);
                    p.gender = f[3];
                    p.phone = f[4];

                    p.spo2 = 0;
                    p.heartRate = 0;
                    p.complaint = "";

                    p.condition = normalizePriority(f[5]);

                    if (p.condition.empty())
                        p.condition = "LOW";

                    p.priority = p.condition;
                    p.bedId = f[7];
                    p.status = f[8];
                    p.registeredAt = f[9];

                    patients.push_back(p);

                    nextPatientId =
                        max(nextPatientId, p.id + 1);
                }
            }
        }

        // beds.txt:
        // id|ward|type|status|patientId
        {
            ifstream in(BED_FILE);
            string line;

            while (getline(in, line)) {

                if (line.empty())
                    continue;

                vector<string> f;
                string x;
                stringstream ss(line);

                while (getline(ss, x, '|'))
                    f.push_back(x);

                if (f.size() < 5)
                    continue;

                Bed b;

                b.id = f[0];
                b.ward = f[1];
                b.type = f[2];
                b.status = f[3];
                b.patientId = toInt(f[4]);

                beds.push_back(b);
            }
        }

        // resources.txt:
        // id|name|category|total|available
        {
            ifstream in(RESOURCE_FILE);
            string line;

            while (getline(in, line)) {

                if (line.empty())
                    continue;

                vector<string> f;
                string x;
                stringstream ss(line);

                while (getline(ss, x, '|'))
                    f.push_back(x);

                if (f.size() < 5)
                    continue;

                Resource r;

                r.id = toInt(f[0]);
                r.name = f[1];
                r.category = f[2];
                r.total = toInt(f[3]);
                r.available = toInt(f[4]);

                resources.push_back(r);

                nextResourceId =
                    max(nextResourceId, r.id + 1);
            }
        }
    }

    // =========================== SAVE FILES ============================

    void savePatientsUnlocked() {

        ofstream out(PATIENT_FILE, ios::trunc);

        for (const auto& p : patients) {

            out << p.id << '|'
                << p.name << '|'
                << p.age << '|'
                << p.gender << '|'
                << p.phone << '|'
                << p.spo2 << '|'
                << p.heartRate << '|'
                << p.complaint << '|'
                << p.condition << '|'
                << p.priority << '|'
                << p.bedId << '|'
                << p.status << '|'
                << p.registeredAt
                << '\n';
        }
    }

    void saveBedsUnlocked() {

        ofstream out(BED_FILE, ios::trunc);

        for (const auto& b : beds) {

            out << b.id << '|'
                << b.ward << '|'
                << b.type << '|'
                << b.status << '|'
                << b.patientId
                << '\n';
        }
    }

    void saveResourcesUnlocked() {

        ofstream out(RESOURCE_FILE, ios::trunc);

        for (const auto& r : resources) {

            out << r.id << '|'
                << r.name << '|'
                << r.category << '|'
                << r.total << '|'
                << r.available
                << '\n';
        }
    }

    // ======================= BED PREFERENCE ============================

    string preferredWard(const Patient& p) const {

        if (p.priority == "CRITICAL")
            return "ICU";

        if (p.priority == "HIGH")
            return "Emergency";

        return "General";
    }

    // ===================== PATIENT REGISTRATION ========================

    string registerPatient(const map<string, string>& f) {

        lock_guard<mutex> lock(mtx);

        string name =
            trim(f.count("name") ? f.at("name") : "");

        if (name.empty())
            return "ERROR|Patient name is required.";

        string condition =
            normalizePriority(
                f.count("condition")
                ? f.at("condition")
                : ""
            );

        if (condition.empty())
            return "ERROR|Please select CRITICAL, HIGH, MEDIUM or LOW.";

        Patient p;

        p.id = nextPatientId++;
        p.name = name;
        p.age = toInt(f.count("age") ? f.at("age") : "0");
        p.gender = f.count("gender") ? f.at("gender") : "";
        p.phone = f.count("phone") ? f.at("phone") : "";

        // These are STORED clinical details.
        p.spo2 = toDouble(
            f.count("spo2") ? f.at("spo2") : "0"
        );

        p.heartRate = toInt(
            f.count("heartRate")
            ? f.at("heartRate")
            : "0"
        );

        p.complaint =
            f.count("complaint")
            ? f.at("complaint")
            : "";

        // IMPORTANT:
        // Condition dropdown is the ONLY source of priority.
        p.condition = condition;
        p.priority = condition;

        p.bedId = "";
        p.status = "Waiting";
        p.registeredAt = nowString();

        patients.push_back(p);

        auditUnlocked(
            "Registered patient " +
            to_string(p.id) +
            " | condition=" +
            p.condition +
            " | priority=" +
            p.priority
        );

        // Automatic bed allocation.
        // The order depends ONLY on selected condition.
        vector<string> order;

        if (p.priority == "CRITICAL") {
            order = { "ICU", "Emergency", "General" };
        }
        else if (p.priority == "HIGH") {
            order = { "Emergency", "ICU", "General" };
        }
        else {
            order = { "General", "Emergency", "ICU" };
        }

        string allocatedBed = "NO_BED";

        for (const string& ward : order) {

            for (auto& b : beds) {

                if (
                    b.ward == ward &&
                    b.status == "Available"
                    ) {

                    b.status = "Occupied";
                    b.patientId = p.id;

                    patients.back().bedId = b.id;
                    patients.back().status = "Admitted";

                    allocatedBed = b.id;

                    auditUnlocked(
                        "Allocated bed " +
                        b.id +
                        " to patient " +
                        to_string(p.id)
                    );

                    break;
                }
            }

            if (allocatedBed != "NO_BED")
                break;
        }

        savePatientsUnlocked();
        saveBedsUnlocked();

        stringstream result;

        result << "OK|"
            << p.id << "|"
            << p.priority << "|"
            << allocatedBed;

        return result.str();
    }

    // ============================ DISCHARGE ============================

    string dischargePatient(int patientId) {

        lock_guard<mutex> lock(mtx);

        auto pit =
            find_if(
                patients.begin(),
                patients.end(),
                [patientId](const Patient& p) {
                    return p.id == patientId;
                }
            );

        if (pit == patients.end())
            return "ERROR|Patient not found.";

        if (pit->status == "Discharged")
            return "ERROR|Patient is already discharged.";

        // Remember the bed recorded on the patient.
        string oldBed = pit->bedId;

        // Release the patient's recorded bed.
        bool bedReleased = false;

        if (!oldBed.empty()) {
            for (auto& b : beds) {
                if (b.id == oldBed) {
                    b.status = "Available";
                    b.patientId = 0;
                    bedReleased = true;
                    break;
                }
            }
        }

        // Safety/recovery check: if the patient's bedId was missing or stale,
        // also look for a bed whose patientId still points to this patient.
        // This prevents an occupied bed from remaining occupied because of
        // inconsistent patient/bed data.
        if (!bedReleased) {
            for (auto& b : beds) {
                if (b.patientId == patientId) {
                    if (oldBed.empty())
                        oldBed = b.id;

                    b.status = "Available";
                    b.patientId = 0;
                    bedReleased = true;
                    break;
                }
            }
        }

        // A discharged patient must never retain a bed assignment.
        pit->bedId = "";
        pit->status = "Discharged";

        auditUnlocked(
            "Discharged patient " +
            to_string(patientId) +
            " | released bed " +
            oldBed
        );

        savePatientsUnlocked();
        saveBedsUnlocked();

        return "OK|Patient discharged and bed released.";
    }

    // ======================== MANUAL BED STATUS ========================

    string setBedStatus(
        const string& bedId,
        const string& status
    ) {

        lock_guard<mutex> lock(mtx);

        if (
            status != "Available" &&
            status != "Reserved" &&
            status != "Maintenance"
            ) {
            return "ERROR|Invalid bed status.";
        }

        for (auto& b : beds) {

            if (b.id == bedId) {

                if (
                    b.status == "Occupied" &&
                    status != "Available"
                    ) {
                    return
                        "ERROR|Occupied bed must be released by discharging the patient.";
                }

                b.status = status;

                if (status == "Available")
                    b.patientId = 0;

                auditUnlocked(
                    "Bed " +
                    bedId +
                    " changed to " +
                    status
                );

                saveBedsUnlocked();

                return "OK|Bed status updated.";
            }
        }

        return "ERROR|Bed not found.";
    }

    // ========================== RESOURCES ==============================

    string updateResource(
        int id,
        int total,
        int available
    ) {

        lock_guard<mutex> lock(mtx);

        if (
            total < 0 ||
            available < 0 ||
            available > total
            ) {
            return
                "ERROR|Available quantity must be between 0 and total.";
        }

        for (auto& r : resources) {

            if (r.id == id) {

                r.total = total;
                r.available = available;

                auditUnlocked(
                    "Updated resource " +
                    r.name
                );

                saveResourcesUnlocked();

                return "OK|Resource updated.";
            }
        }

        return "ERROR|Resource not found.";
    }

    // ============================ JSON ================================

    string dashboardJson() {

        lock_guard<mutex> lock(mtx);

        int available = 0;
        int occupied = 0;
        int reserved = 0;
        int maintenance = 0;

        int icuAvailable = 0;
        int emergencyAvailable = 0;
        int generalAvailable = 0;

        for (const auto& b : beds) {

            if (b.status == "Available") {

                ++available;

                if (b.ward == "ICU")
                    ++icuAvailable;

                else if (b.ward == "Emergency")
                    ++emergencyAvailable;

                else if (b.ward == "General")
                    ++generalAvailable;
            }
            else if (b.status == "Occupied") {
                ++occupied;
            }
            else if (b.status == "Reserved") {
                ++reserved;
            }
            else if (b.status == "Maintenance") {
                ++maintenance;
            }
        }

        int active = 0;
        int waiting = 0;

        int critical = 0;
        int high = 0;
        int medium = 0;
        int low = 0;

        for (const auto& p : patients) {

            if (p.status != "Discharged")
                ++active;

            if (p.status == "Waiting")
                ++waiting;

            if (
                p.priority == "CRITICAL" &&
                p.status != "Discharged"
                )
                ++critical;

            if (
                p.priority == "HIGH" &&
                p.status != "Discharged"
                )
                ++high;

            if (
                p.priority == "MEDIUM" &&
                p.status != "Discharged"
                )
                ++medium;

            if (
                p.priority == "LOW" &&
                p.status != "Discharged"
                )
                ++low;
        }

        stringstream j;

        j << "{";

        j << "\"beds\":{"
            << "\"total\":" << beds.size() << ","
            << "\"available\":" << available << ","
            << "\"occupied\":" << occupied << ","
            << "\"reserved\":" << reserved << ","
            << "\"maintenance\":" << maintenance << ","
            << "\"icuAvailable\":" << icuAvailable << ","
            << "\"emergencyAvailable\":" << emergencyAvailable << ","
            << "\"generalAvailable\":" << generalAvailable
            << "},";

        j << "\"patients\":{"
            << "\"total\":" << patients.size() << ","
            << "\"active\":" << active << ","
            << "\"waiting\":" << waiting << ","
            << "\"critical\":" << critical << ","
            << "\"high\":" << high << ","
            << "\"medium\":" << medium << ","
            << "\"low\":" << low
            << "},";

        j << "\"updated\":\""
            << jsonEscape(nowString())
            << "\"";

        j << "}";

        return j.str();
    }

    string patientsJson() {

        lock_guard<mutex> lock(mtx);

        stringstream j;

        j << "[";

        for (size_t i = 0; i < patients.size(); ++i) {

            if (i)
                j << ",";

            const auto& p = patients[i];

            j << "{"
                << "\"id\":" << p.id << ","
                << "\"name\":\""
                << jsonEscape(p.name)
                << "\","
                << "\"age\":" << p.age << ","
                << "\"gender\":\""
                << jsonEscape(p.gender)
                << "\","
                << "\"phone\":\""
                << jsonEscape(p.phone)
                << "\","
                << "\"spo2\":"
                << fixed << setprecision(1)
                << p.spo2
                << ","
                << "\"heartRate\":"
                << p.heartRate
                << ","
                << "\"complaint\":\""
                << jsonEscape(p.complaint)
                << "\","
                << "\"condition\":\""
                << jsonEscape(p.condition)
                << "\","
                << "\"priority\":\""
                << jsonEscape(p.priority)
                << "\","
                << "\"bed\":\""
                << jsonEscape(p.bedId)
                << "\","
                << "\"status\":\""
                << jsonEscape(p.status)
                << "\","
                << "\"registered\":\""
                << jsonEscape(p.registeredAt)
                << "\""
                << "}";
        }

        j << "]";

        return j.str();
    }

    string bedsJson() {

        lock_guard<mutex> lock(mtx);

        stringstream j;

        j << "[";

        for (size_t i = 0; i < beds.size(); ++i) {

            if (i)
                j << ",";

            const auto& b = beds[i];

            j << "{"
                << "\"id\":\""
                << jsonEscape(b.id)
                << "\","
                << "\"ward\":\""
                << jsonEscape(b.ward)
                << "\","
                << "\"type\":\""
                << jsonEscape(b.type)
                << "\","
                << "\"status\":\""
                << jsonEscape(b.status)
                << "\","
                << "\"patientId\":"
                << b.patientId
                << "}";
        }

        j << "]";

        return j.str();
    }

    string resourcesJson() {

        lock_guard<mutex> lock(mtx);

        stringstream j;

        j << "[";

        for (size_t i = 0; i < resources.size(); ++i) {

            if (i)
                j << ",";

            const auto& r = resources[i];

            j << "{"
                << "\"id\":" << r.id << ","
                << "\"name\":\""
                << jsonEscape(r.name)
                << "\","
                << "\"category\":\""
                << jsonEscape(r.category)
                << "\","
                << "\"total\":"
                << r.total
                << ","
                << "\"available\":"
                << r.available
                << "}";
        }

        j << "]";

        return j.str();
    }

    string reportText() {

        lock_guard<mutex> lock(mtx);

        int available = 0;
        int occupied = 0;
        int reserved = 0;
        int maintenance = 0;

        for (const auto& b : beds) {

            if (b.status == "Available")
                ++available;
            else if (b.status == "Occupied")
                ++occupied;
            else if (b.status == "Reserved")
                ++reserved;
            else
                ++maintenance;
        }

        int active = 0;
        int discharged = 0;
        int waiting = 0;

        for (const auto& p : patients) {

            if (p.status == "Discharged")
                ++discharged;
            else
                ++active;

            if (p.status == "Waiting")
                ++waiting;
        }

        stringstream r;

        r << "HOSPITAL RESOURCE & PATIENT MANAGEMENT REPORT\n";
        r << "Generated: " << nowString() << "\n";
        r << "=============================================\n\n";

        r << "BED CAPACITY\n";
        r << "Total beds:       " << beds.size() << "\n";
        r << "Available:        " << available << "\n";
        r << "Occupied:         " << occupied << "\n";
        r << "Reserved:         " << reserved << "\n";
        r << "Maintenance:      " << maintenance << "\n\n";

        r << "PATIENTS\n";
        r << "Total registered: " << patients.size() << "\n";
        r << "Active:           " << active << "\n";
        r << "Waiting:          " << waiting << "\n";
        r << "Discharged:       " << discharged << "\n\n";

        r << "PRIORITY RULE\n";
        r << "Priority is assigned ONLY from the selected Condition.\n";
        r << "SpO2, Heart Rate and Complaint are stored as clinical details.\n\n";

        r << "RESOURCE INVENTORY\n";

        for (const auto& x : resources) {

            r << x.name
                << " ("
                << x.category
                << "): "
                << x.available
                << "/"
                << x.total
                << " available\n";
        }

        r << "\nACTIVE PATIENTS\n";

        for (const auto& p : patients) {

            if (p.status == "Discharged")
                continue;

            r << "#"
                << p.id
                << " | "
                << p.name
                << " | SpO2: "
                << p.spo2
                << " | HR: "
                << p.heartRate
                << " | Complaint: "
                << p.complaint
                << " | Condition: "
                << p.condition
                << " | Priority: "
                << p.priority
                << " | Bed: "
                << (p.bedId.empty() ? "WAITING" : p.bedId)
                << "\n";
        }

        return r.str();
    }
};

// =============================== HTML =================================

const string HTML_PAGE = R"HTML(
<!DOCTYPE html>
<html lang="en">

<head>

<meta charset="UTF-8">

<meta name="viewport"
      content="width=device-width, initial-scale=1.0">

<title>Hospital Resource & Patient Management</title>

<style>

* {
    box-sizing: border-box;
}

body {
    margin: 0;
    font-family: Segoe UI, Arial, sans-serif;
    background: #f4f7fb;
    color: #172033;
}

header {
    background: linear-gradient(135deg, #0b3b66, #1479a8);
    color: white;
    padding: 22px 28px;
}

header h1 {
    margin: 0;
    font-size: 26px;
}

header p {
    margin: 6px 0 0;
    opacity: .92;
}

nav {
    display: flex;
    flex-wrap: wrap;
    gap: 8px;
    padding: 12px 20px;
    background: white;
    box-shadow: 0 2px 8px #0001;
    position: sticky;
    top: 0;
    z-index: 5;
}

nav button {
    border: 0;
    background: #eaf1f7;
    padding: 10px 15px;
    border-radius: 8px;
    cursor: pointer;
    font-weight: 600;
}

nav button.active,
nav button:hover {
    background: #1479a8;
    color: white;
}

main {
    max-width: 1450px;
    margin: auto;
    padding: 22px;
}

section {
    display: none;
}

section.active {
    display: block;
}

.cards {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
    gap: 15px;
    margin-bottom: 20px;
}

.card {
    background: white;
    border-radius: 14px;
    padding: 18px;
    box-shadow: 0 3px 12px #00000012;
}

.card .num {
    font-size: 30px;
    font-weight: 800;
    margin-top: 8px;
}

.card .label {
    color: #65748b;
}

.grid2 {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 18px;
}

.panel {
    background: white;
    border-radius: 14px;
    padding: 20px;
    box-shadow: 0 3px 12px #00000012;
    margin-bottom: 18px;
}

.panel h2 {
    margin-top: 0;
}

form {
    display: grid;
    grid-template-columns: repeat(2, 1fr);
    gap: 13px;
}

label {
    font-size: 13px;
    font-weight: 700;
    color: #526174;
}

input,
select,
textarea {
    width: 100%;
    padding: 11px;
    border: 1px solid #ccd6e2;
    border-radius: 8px;
    margin-top: 5px;
    background: white;
    font-family: inherit;
}

textarea {
    min-height: 85px;
    resize: vertical;
}

.full {
    grid-column: 1 / -1;
}

button.primary {
    background: #1479a8;
    color: white;
    border: 0;
    padding: 11px 16px;
    border-radius: 8px;
    font-weight: 700;
    cursor: pointer;
}

button.danger {
    background: #c83c3c;
    color: white;
    border: 0;
    padding: 7px 10px;
    border-radius: 6px;
    cursor: pointer;
}

button.small {
    background: #eef3f7;
    border: 0;
    padding: 7px 9px;
    border-radius: 6px;
    cursor: pointer;
}

table {
    width: 100%;
    border-collapse: collapse;
    font-size: 13px;
}

th,
td {
    text-align: left;
    padding: 9px;
    border-bottom: 1px solid #edf0f4;
    vertical-align: top;
}

th {
    background: #f6f8fa;
}

.badge {
    padding: 4px 8px;
    border-radius: 999px;
    font-weight: 700;
    font-size: 11px;
}

.CRITICAL {
    background: #ffd8d8;
    color: #a40000;
}

.HIGH {
    background: #ffe7c2;
    color: #9a5200;
}

.MEDIUM {
    background: #fff4b8;
    color: #765f00;
}

.LOW {
    background: #dcf6df;
    color: #18702b;
}

.notice {
    padding: 13px;
    border-radius: 8px;
    background: #eef7ff;
    margin-bottom: 14px;
}

.condition-box {
    border: 2px solid #1479a8;
    border-radius: 10px;
    padding: 15px;
    background: #f3fbff;
}

.condition-box select {
    font-size: 16px;
    font-weight: 700;
}

#priorityPreview {
    margin-top: 10px;
    font-weight: 800;
}

.patient-details-box {
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    gap: 10px;
    margin-top: 8px;
}

.detail {
    background: #f7f9fb;
    padding: 10px;
    border-radius: 8px;
}

.detail b {
    display: block;
    margin-bottom: 3px;
}

.bed-section-title {
    margin-top: 22px;
    padding: 12px 14px;
    background: #f6f8fa;
    border-radius: 9px;
    font-weight: 800;
}

.bedgrid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(112px, 1fr));
    gap: 9px;
    margin-top: 12px;
}

.bed {
    min-height: 76px;
    padding: 10px;
    border-radius: 9px;
    color: white;
    text-align: center;
    cursor: pointer;
    font-size: 12px;
    font-weight: 600;
    transition: transform .12s, opacity .12s;
}

.bed:hover {
    transform: translateY(-2px);
    opacity: .9;
}

.bed-menu {
    position: absolute;
    background: white;
    border-radius: 8px;
    box-shadow: 0 6px 20px rgba(0,0,0,0.18);
    padding: 8px;
    min-width: 180px;
    z-index: 9999;
    transform-origin: top left;
    animation: pop .14s ease-out;
}

.bed-menu h4 {
    margin: 6px 8px;
    font-size: 13px;
    color: #233242;
}

.bed-menu button {
    display: block;
    width: 100%;
    text-align: left;
    border: 0;
    background: transparent;
    padding: 8px 10px;
    border-radius: 6px;
    cursor: pointer;
    font-weight: 700;
}

.bed-menu button:hover {
    background: #f3f6fb;
}

@keyframes pop {
    from { transform: scale(.92); opacity: 0; }
    to   { transform: scale(1); opacity: 1; }
}

.available {
    background: #239b55;
}

.occupied {
    background: #d53939;
}

.reserved {
    background: #d58b24;
}

.maintenance {
    background: #697586;
}

.legend {
    display: flex;
    flex-wrap: wrap;
    gap: 14px;
    margin: 12px 0;
}

.legend-item {
    display: flex;
    align-items: center;
    gap: 6px;
    font-size: 13px;
    font-weight: 700;
}

.legend-box {
    width: 18px;
    height: 18px;
    border-radius: 4px;
}

.legend-available {
    background: #239b55;
}

.legend-occupied {
    background: #d53939;
}

.legend-reserved {
    background: #d58b24;
}

.legend-maintenance {
    background: #697586;
}

pre {
    white-space: pre-wrap;
    background: #0d1b2a;
    color: #d8e9f7;
    padding: 15px;
    border-radius: 9px;
    max-height: 500px;
    overflow: auto;
}

@media(max-width: 900px) {

    .grid2 {
        grid-template-columns: 1fr;
    }

    form {
        grid-template-columns: 1fr;
    }

    .full {
        grid-column: auto;
    }

    .patient-details-box {
        grid-template-columns: 1fr;
    }
}

</style>

</head>

<body>

<header>

<h1>🏥 Hospital Resource & Patient Management System</h1>

<p>
Patient Registration • Clinical Details • Condition-Based Priority •
50-Bed Management • Resources • Reports
</p>

</header>

<nav>

<button class="tab active"
        onclick="showTab('dashboard', this)">
    Dashboard
</button>

<button class="tab"
        onclick="showTab('register', this)">
    Register Patient
</button>

<button class="tab"
        onclick="showTab('patients', this)">
    Patient Details
</button>

<button class="tab"
        onclick="showTab('beds', this)">
    Bed Management
</button>

<button class="tab"
        onclick="showTab('resources', this)">
    Resources
</button>

<button class="tab"
        onclick="showTab('reports', this)">
    Admin Reports
</button>

</nav>

<main>

<!-- ========================= DASHBOARD ========================== -->

<section id="dashboard" class="active">

<div class="cards">

<div class="card">
    <div class="label">Total Beds</div>
    <div class="num" id="totalBeds">50</div>
</div>

<div class="card">
    <div class="label">Available Beds</div>
    <div class="num" id="availableBeds">0</div>
</div>

<div class="card">
    <div class="label">Occupied Beds</div>
    <div class="num" id="occupiedBeds">0</div>
</div>

<div class="card">
    <div class="label">Active Patients</div>
    <div class="num" id="activePatients">0</div>
</div>

<div class="card">
    <div class="label">Waiting Patients</div>
    <div class="num" id="waitingPatients">0</div>
</div>

<div class="card">
    <div class="label">Critical Cases</div>
    <div class="num" id="criticalPatients">0</div>
</div>

</div>

<div class="grid2">

<div class="panel">

<h2>Ward Availability</h2>

<table>

<tr>
    <th>Ward</th>
    <th>Available</th>
    <th>Total</th>
</tr>

<tr>
    <td>ICU</td>
    <td id="icuAvail">0</td>
    <td>10</td>
</tr>

<tr>
    <td>Emergency</td>
    <td id="erAvail">0</td>
    <td>15</td>
</tr>

<tr>
    <td>General</td>
    <td id="genAvail">0</td>
    <td>25</td>
</tr>

</table>

</div>

<div class="panel">

<h2>Priority Summary</h2>

<table>

<tr>
    <td>CRITICAL</td>
    <td id="critical2">0</td>
</tr>

<tr>
    <td>HIGH</td>
    <td id="high2">0</td>
</tr>

<tr>
    <td>MEDIUM</td>
    <td id="medium2">0</td>
</tr>

<tr>
    <td>LOW</td>
    <td id="low2">0</td>
</tr>

</table>

</div>

</div>

<div class="panel">

<h2>How Priority Works</h2>

<div class="notice">

<b>The Condition dropdown is the only thing that assigns priority.</b>

<br><br>

CRITICAL → Priority CRITICAL → Preferred ICU

<br>
HIGH → Priority HIGH → Preferred Emergency

<br>
MEDIUM → Priority MEDIUM → Preferred General

<br>
LOW → Priority LOW → Preferred General

<br><br>

SpO₂, Heart Rate and Complaint are stored as
patient clinical details and do not automatically change the selected priority.

</div>

Last update:
<span id="updated">-</span>

</div>

</section>

<!-- ========================= REGISTER ========================== -->

<section id="register">

<div class="panel">

<h2>Patient Registration</h2>

<div class="notice">

Enter the patient's clinical details below.
Then select the patient's <b>Condition</b>.
The selected condition directly becomes the priority.

</div>

<form id="patientForm">

<div>

<label>
Patient Name

<input
    name="name"
    required
    placeholder="Enter patient's full name">

</label>

</div>

<div>

<label>
Age

<input
    name="age"
    type="number"
    min="0"
    max="120"
    required
    placeholder="Enter age">

</label>

</div>

<div>

<label>
Gender

<select name="gender">

<option value="Male">Male</option>
<option value="Female">Female</option>
<option value="Other">Other</option>

</select>

</label>

</div>

<div>

<label>
Phone

<input
    name="phone"
    placeholder="Enter phone number">

</label>

</div>

<div>

<label>
SpO₂ (%)

<input
    name="spo2"
    type="number"
    step="0.1"
    min="0"
    max="100"
    placeholder="Example: 97">

</label>

</div>

<div>

<label>
Heart Rate (BPM)

<input
    name="heartRate"
    type="number"
    min="0"
    max="300"
    placeholder="Example: 82">

</label>

</div>

<div class="full">

<label>
Complaint

<textarea
    name="complaint"
    placeholder="Enter patient's complaint / symptoms"></textarea>

</label>

</div>

<div class="full">

<div class="condition-box">

<label>
Condition

<select
    name="condition"
    id="condition"
    required
    onchange="updatePriorityPreview()">

<option value="">
-- Select Condition --
</option>

<option value="CRITICAL">
CRITICAL
</option>

<option value="HIGH">
HIGH
</option>

<option value="MEDIUM">
MEDIUM
</option>

<option value="LOW">
LOW
</option>

</select>

</label>

<div id="priorityPreview">
Please select a condition.
</div>

</div>

</div>

<div class="full">

<button class="primary" type="submit">
Register Patient & Allocate Bed
</button>

</div>

</form>

</div>

</section>

<!-- ========================== PATIENTS ========================= -->

<section id="patients">

<div class="panel">

<h2>Patient Details</h2>

<div class="notice">

This table stores and displays the patient's
<b>SpO₂, Heart Rate and Complaint</b> separately from
the selected Condition and Priority.

</div>

<button class="small" onclick="loadPatients()">
Refresh
</button>

<div style="overflow:auto; margin-top:12px;">

<table>

<thead>

<tr>

<th>ID</th>
<th>Patient</th>
<th>SpO₂</th>
<th>Heart Rate</th>
<th>Complaint</th>
<th>Condition</th>
<th>Priority</th>
<th>Bed</th>
<th>Status</th>
<th>Action</th>

</tr>

</thead>

<tbody id="patientTable">
</tbody>

</table>

</div>

</div>

</section>

<!-- ============================ BEDS ============================ -->

<section id="beds">

<div class="panel">

<h2>50-Bed Management</h2>

<div class="notice">

<b>All 50 beds are shown below.</b>

<br>

🟩 Green = Available / Free

<br>
🟥 Red = Occupied

<br>
🟧 Orange = Reserved

<br>
⬜ Grey = Maintenance

<br><br>

When a patient is admitted, their bed automatically turns
<b>red</b>. When they are discharged, the bed automatically
turns <b>green</b> again.

</div>

<div class="legend">

<div class="legend-item">
    <span class="legend-box legend-available"></span>
    Available
</div>

<div class="legend-item">
    <span class="legend-box legend-occupied"></span>
    Occupied
</div>

<div class="legend-item">
    <span class="legend-box legend-reserved"></span>
    Reserved
</div>

<div class="legend-item">
    <span class="legend-box legend-maintenance"></span>
    Maintenance
</div>

</div>

<div class="bed-section-title">
    ICU — 10 Beds
</div>

<div class="bedgrid" id="icuGrid"></div>

<div class="bed-section-title">
    Emergency — 15 Beds
</div>

<div class="bedgrid" id="erGrid"></div>

<div class="bed-section-title">
    General — 25 Beds
</div>

<div class="bedgrid" id="genGrid"></div>

</div>

</section>

<!-- ========================= RESOURCES ========================= -->

<section id="resources">

<div class="panel">

<h2>Resource & Equipment Management</h2>

<table>

<thead>

<tr>
<th>Resource</th>
<th>Category</th>
<th>Total</th>
<th>Available</th>
<th>Update</th>
</tr>

</thead>

<tbody id="resourceTable"></tbody>

</table>

</div>

</section>

<!-- =========================== REPORT ========================== -->

<section id="reports">

<div class="panel">

<h2>Admin Report</h2>

<button class="primary" onclick="loadReport()">
Generate Report
</button>

<pre id="reportBox">
Click Generate Report.
</pre>

</div>

</section>

</main>

<script>

// ============================ API =================================

async function api(url, options = {}) {

    // Always request fresh GET data so the UI cannot keep an old bed status
    // such as "Occupied" after a patient has been discharged.
    const requestOptions = { ...options };

    if (!requestOptions.method || requestOptions.method.toUpperCase() === 'GET') {
        requestOptions.cache = 'no-store';
    }

    const r = await fetch(url, requestOptions);

    return await r.text();
}

// =========================== TABS ================================

function showTab(id, btn) {

    document
        .querySelectorAll('section')
        .forEach(x => x.classList.remove('active'));

    document
        .getElementById(id)
        .classList.add('active');

    document
        .querySelectorAll('.tab')
        .forEach(x => x.classList.remove('active'));

    btn.classList.add('active');

    if (id === 'patients')
        loadPatients();

    if (id === 'beds')
        loadBeds();

    if (id === 'resources')
        loadResources();
}

// ========================= PRIORITY ===============================

function updatePriorityPreview() {

    const value =
        document.getElementById('condition').value;

    const box =
        document.getElementById('priorityPreview');

    if (!value) {

        box.textContent =
            'Please select a condition.';

        return;
    }

    if (value === 'CRITICAL') {

        box.textContent =
            'Priority: CRITICAL — preferred ward: ICU';

    }
    else if (value === 'HIGH') {

        box.textContent =
            'Priority: HIGH — preferred ward: Emergency';

    }
    else if (value === 'MEDIUM') {

        box.textContent =
            'Priority: MEDIUM — preferred ward: General';

    }
    else {

        box.textContent =
            'Priority: LOW — preferred ward: General';

    }
}

// ========================== DASHBOARD =============================

async function loadDashboard() {

    try {

        const d =
            JSON.parse(
                await api('/api/dashboard')
            );

        document.getElementById('totalBeds')
            .textContent = d.beds.total;

        document.getElementById('availableBeds')
            .textContent = d.beds.available;

        document.getElementById('occupiedBeds')
            .textContent = d.beds.occupied;

        document.getElementById('activePatients')
            .textContent = d.patients.active;

        document.getElementById('waitingPatients')
            .textContent = d.patients.waiting;

        document.getElementById('criticalPatients')
            .textContent = d.patients.critical;

        document.getElementById('icuAvail')
            .textContent = d.beds.icuAvailable;

        document.getElementById('erAvail')
            .textContent = d.beds.emergencyAvailable;

        document.getElementById('genAvail')
            .textContent = d.beds.generalAvailable;

        document.getElementById('critical2')
            .textContent = d.patients.critical;

        document.getElementById('high2')
            .textContent = d.patients.high;

        document.getElementById('medium2')
            .textContent = d.patients.medium;

        document.getElementById('low2')
            .textContent = d.patients.low;

        document.getElementById('updated')
            .textContent = d.updated;

    }
    catch (e) {

        console.log('Dashboard error:', e);

    }
}

// ========================= REGISTER ===============================

document
    .getElementById('patientForm')
    .addEventListener(
        'submit',
        async function(e) {

            e.preventDefault();

            const condition =
                document.getElementById('condition').value;

            if (!condition) {

                alert(
                    'Please select the patient condition.'
                );

                return;
            }

            const f =
                new FormData(e.target);

            const text =
                await api(
                    '/api/register',
                    {
                        method: 'POST',
                        body:
                            new URLSearchParams(f)
                    }
                );

            const x = text.split('|');

            if (x[0] === 'OK') {

                const patientId = x[1];
                const priority = x[2];
                const bed = x[3];

                alert(
                    'Patient registered successfully!\\n\\n' +
                    'Patient ID: ' + patientId + '\\n' +
                    'Condition/Priority: ' + priority + '\\n' +
                    'Allocated Bed: ' +
                    (
                        bed === 'NO_BED'
                        ? 'No bed available — patient is waiting'
                        : bed
                    )
                );

                e.target.reset();

                document.getElementById(
                    'priorityPreview'
                ).textContent =
                    'Please select a condition.';

                loadDashboard();
                loadPatients();
                loadBeds();

            }
            else {

                alert(
                    text.replace('|', ' ')
                );
            }
        }
    );

// =========================== PATIENTS =============================

async function loadPatients() {

    try {

        const data =
            JSON.parse(
                await api('/api/patients')
            );

        const body =
            document.getElementById(
                'patientTable'
            );

        body.innerHTML = '';

        const rank = {
            'CRITICAL': 4,
            'HIGH': 3,
            'MEDIUM': 2,
            'LOW': 1
        };

        data.sort(
            (a, b) =>
                rank[b.priority] -
                rank[a.priority]
        );

        for (const p of data) {

            const tr =
                document.createElement('tr');

            tr.innerHTML = `

                <td>${p.id}</td>

                <td>
                    <b>${esc(p.name)}</b>
                    <br>
                    Age: ${p.age}
                    <br>
                    Gender: ${esc(p.gender)}
                    <br>
                    Phone: ${esc(p.phone)}
                </td>

                <td>
                    ${p.spo2}%
                </td>

                <td>
                    ${p.heartRate} BPM
                </td>

                <td>
                    ${esc(p.complaint)}
                </td>

                <td>
                    ${p.condition}
                </td>

                <td>
                    <span class="badge ${p.priority}">
                        ${p.priority}
                    </span>
                </td>

                <td>
                    ${p.bed || 'WAITING'}
                </td>

                <td>
                    ${p.status}
                </td>

                <td>
                    ${
                        p.status !== 'Discharged'
                        ?
                        `<button
                            class="danger"
                            onclick="discharge(${p.id})">
                            Discharge
                        </button>`
                        :
                        ''
                    }
                </td>
            `;

            body.appendChild(tr);
        }

    }
    catch (e) {

        console.log('Patient loading error:', e);

    }
}

async function discharge(id, skipConfirm = false) {

    if (!skipConfirm) {
        if (!confirm('Discharge patient #' + id + '?'))
            return;
    }

    try {
        const text =
            await api(
                '/api/discharge',
                {
                    method: 'POST',
                    body:
                        new URLSearchParams({
                            id: id
                        })
                }
            );

        alert(
            text.replace('|', ' ')
        );

        // Wait for the server-backed data to be refreshed before the user
        // can click a bed. This removes stale "Occupied" bed objects.
        await loadPatients();
        await loadDashboard();
        await loadBeds();

    } catch (e) {
        console.error('Discharge error:', e);
        alert('Unable to discharge patient.');
    }
}

// ============================= BEDS ===============================

async function loadBeds() {

    try {

        const data =
            JSON.parse(
                await api('/api/beds')
            );

        // Explicitly clear all three sections.
        document.getElementById('icuGrid').innerHTML = '';
        document.getElementById('erGrid').innerHTML = '';
        document.getElementById('genGrid').innerHTML = '';

        // Make sure every returned bed is rendered.
        for (const b of data) {

            const d =
                document.createElement('div');

            d.className =
                'bed ' +
                statusClass(b.status);

            d.innerHTML =
                '<b style="font-size:15px;">' +
                esc(b.id) +
                '</b>' +
                '<br>' +
                esc(b.status) +
                (
                    b.patientId
                    ? '<br>Patient #' +
                      b.patientId
                    : ''
                );

            d.title =
                b.id +
                ' | ' +
                b.status +
                (
                    b.patientId
                    ? ' | Patient #' + b.patientId
                    : ''
                );

            d.onclick = (ev) => showBedMenu(ev, d, b);

            if (b.ward === 'ICU') {

                document
                    .getElementById('icuGrid')
                    .appendChild(d);

            }
            else if (b.ward === 'Emergency') {

                document
                    .getElementById('erGrid')
                    .appendChild(d);

            }
            else {

                document
                    .getElementById('genGrid')
                    .appendChild(d);
            }
        }

    }
    catch (e) {

        console.log(
            'Bed loading error:',
            e
        );

    }
}

function statusClass(status) {

    if (status === 'Available')
        return 'available';

    if (status === 'Occupied')
        return 'occupied';

    if (status === 'Reserved')
        return 'reserved';

    return 'maintenance';
}

// Bed action menu: show an animated dropdown with options to set status or
// discharge the occupying patient (if any).

function closeBedMenu() {
    const existing = document.querySelector('.bed-menu');
    if (existing) {
        existing.remove();
    }
    window.removeEventListener('click', closeBedMenu);
}

async function getLatestBed(b) {
    const latestBeds = JSON.parse(await api('/api/beds'));
    return latestBeds.find(bed => bed.id === b.id);
}

async function showBedMenu(ev, elem, b) {
    ev.stopPropagation();
    closeBedMenu();

    try {
        const latest = await getLatestBed(b);
        if (!latest) { alert('Bed not found.'); return; }
        b = latest;
    } catch (e) {
        console.error('Bed refresh error:', e);
        alert('Unable to refresh bed status. Please try again.');
        return;
    }

    const menu = document.createElement('div');
    menu.className = 'bed-menu';

    const title = document.createElement('h4');
    title.textContent = 'Bed ' + b.id + ' — ' + b.status;
    menu.appendChild(title);

    const statuses = ['Available','Reserved','Maintenance'];

    for (const s of statuses) {
        const btn = document.createElement('button');
        btn.textContent = s;
        btn.onclick = async (e) => {
            e.stopPropagation();
            if (b.status === s) { closeBedMenu(); return; }
            if (!confirm('Change ' + b.id + ' from ' + b.status + ' to ' + s + '?')) return;
            try {
                const text = await api('/api/bed-status', {
                    method: 'POST',
                    body: new URLSearchParams({ id: b.id, status: s })
                });
                alert(text.replace('|',' '));
                closeBedMenu();
                await loadBeds();
                await loadDashboard();
                await loadPatients();
            } catch (err) {
                console.error('Set bed status error:', err);
                alert('Unable to change bed status.');
            }
        };
        menu.appendChild(btn);
    }

    if (b.patientId) {
        const sep = document.createElement('hr');
        sep.style.border = 'none';
        sep.style.height = '1px';
        sep.style.background = '#eef3f7';
        sep.style.margin = '8px 0';
        menu.appendChild(sep);

        const disBtn = document.createElement('button');
        disBtn.textContent = 'Discharge Patient #' + b.patientId;
        disBtn.style.color = '#c83c3c';
        disBtn.onclick = (e) => {
            e.stopPropagation();
            closeBedMenu();
            // call discharge skipping the confirm prompt since user selected via menu
            discharge(b.patientId, true);
        };
        menu.appendChild(disBtn);
    }

    document.body.appendChild(menu);

    // Position the menu near the clicked element.
    const r = elem.getBoundingClientRect();
    const left = Math.min(window.innerWidth - 220, r.left + window.scrollX);
    const top = r.bottom + 6 + window.scrollY;
    menu.style.left = left + 'px';
    menu.style.top = top + 'px';

    // Close when clicking outside.
    setTimeout(() => window.addEventListener('click', closeBedMenu), 0);
}

// =========================== RESOURCES =============================

async function loadResources() {

    const data =
        JSON.parse(
            await api('/api/resources')
        );

    const body =
        document.getElementById(
            'resourceTable'
        );

    body.innerHTML = '';

    for (const r of data) {

        const tr =
            document.createElement('tr');

        tr.innerHTML = `

            <td>${esc(r.name)}</td>

            <td>${esc(r.category)}</td>

            <td>
                <input
                    id="tot${r.id}"
                    type="number"
                    value="${r.total}"
                    min="0">
            </td>

            <td>
                <input
                    id="av${r.id}"
                    type="number"
                    value="${r.available}"
                    min="0">
            </td>

            <td>
                <button
                    class="small"
                    onclick="updateResource(${r.id})">
                    Save
                </button>
            </td>
        `;

        body.appendChild(tr);
    }
}

async function updateResource(id) {

    const total =
        document.getElementById(
            'tot' + id
        ).value;

    const available =
        document.getElementById(
            'av' + id
        ).value;

    const text =
        await api(
            '/api/resource',
            {
                method: 'POST',
                body:
                    new URLSearchParams({
                        id: id,
                        total: total,
                        available: available
                    })
            }
        );

    alert(
        text.replace('|', ' ')
    );

    loadResources();
}

// ============================= REPORT ==============================

async function loadReport() {

    document.getElementById(
        'reportBox'
    ).textContent =
        await api('/api/report');
}

// ============================ SECURITY =============================

function esc(s) {

    return String(s ?? '')
        .replace(
            /[&<>"']/g,
            m => ({
                '&': '&amp;',
                '<': '&lt;',
                '>': '&gt;',
                '"': '&quot;',
                "'": '&#39;'
            }[m])
        );
}

// =========================== STARTUP ===============================

loadDashboard();

// Load the beds immediately, even before
// the user clicks the Bed Management tab.
loadBeds();

loadPatients();

setInterval(
    loadDashboard,
    5000
);

setInterval(
    loadBeds,
    5000
);

</script>

</body>
</html>
)HTML";

// =========================== HTTP SERVER =============================

class HttpServer {

private:

    HospitalSystem& hospital;

    SOCKET serverSocket =
        INVALID_SOCKET;

    atomic<bool> running{ false };

    string response(
        const string& status,
        const string& contentType,
        const string& body
    ) {

        stringstream ss;

        ss
            << "HTTP/1.1 "
            << status
            << "\r\n"

            << "Content-Type: "
            << contentType
            << "\r\n"

            << "Content-Length: "
            << body.size()
            << "\r\n"

            << "Cache-Control: no-cache"
            << "\r\n"

            << "Access-Control-Allow-Origin: *"
            << "\r\n"

            << "Connection: close"
            << "\r\n\r\n"

            << body;

        return ss.str();
    }

    string readRequest(SOCKET client) {

        string request;

        char buffer[8192];

        int n =
            recv(
                client,
                buffer,
                sizeof(buffer),
                0
            );

        if (n <= 0)
            return "";

        request.append(
            buffer,
            n
        );

        size_t headerEnd =
            request.find(
                "\r\n\r\n"
            );

        if (
            headerEnd ==
            string::npos
            )
            return request;

        size_t contentLengthPos =
            request.find(
                "Content-Length:"
            );

        int contentLength = 0;

        if (
            contentLengthPos !=
            string::npos
            ) {

            size_t lineEnd =
                request.find(
                    "\r\n",
                    contentLengthPos
                );

            string value =
                request.substr(
                    contentLengthPos + 15,
                    lineEnd -
                    (contentLengthPos + 15)
                );

            contentLength =
                atoi(
                    trim(value).c_str()
                );
        }

        size_t bodyStart =
            headerEnd + 4;

        int already =
            static_cast<int>(
                request.size() -
                bodyStart
                );

        while (
            already <
            contentLength
            ) {

            n =
                recv(
                    client,
                    buffer,
                    sizeof(buffer),
                    0
                );

            if (n <= 0)
                break;

            request.append(
                buffer,
                n
            );

            already += n;
        }

        return request;
    }

    void handleClient(SOCKET client) {

        string req =
            readRequest(client);

        if (req.empty()) {

            closesocket(client);
            return;
        }

        size_t firstLineEnd =
            req.find("\r\n");

        string firstLine =
            req.substr(
                0,
                firstLineEnd
            );

        string method;
        string path;

        {
            stringstream ss(firstLine);
            ss >> method >> path;
        }

        size_t queryPos =
            path.find('?');

        if (
            queryPos !=
            string::npos
            ) {

            path =
                path.substr(
                    0,
                    queryPos
                );
        }

        size_t headerEnd =
            req.find(
                "\r\n\r\n"
            );

        string body =
            (
                headerEnd ==
                string::npos
                ? ""
                : req.substr(
                    headerEnd + 4
                )
                );

        string out;

        // HTML
        if (
            method == "GET" &&
            (
                path == "/" ||
                path == "/index.html"
                )
            ) {

            out =
                response(
                    "200 OK",
                    "text/html; charset=UTF-8",
                    HTML_PAGE
                );
        }

        // Dashboard
        else if (
            method == "GET" &&
            path == "/api/dashboard"
            ) {

            out =
                response(
                    "200 OK",
                    "application/json; charset=UTF-8",
                    hospital.dashboardJson()
                );
        }

        // Patients
        else if (
            method == "GET" &&
            path == "/api/patients"
            ) {

            out =
                response(
                    "200 OK",
                    "application/json; charset=UTF-8",
                    hospital.patientsJson()
                );
        }

        // Beds
        else if (
            method == "GET" &&
            path == "/api/beds"
            ) {

            out =
                response(
                    "200 OK",
                    "application/json; charset=UTF-8",
                    hospital.bedsJson()
                );
        }

        // Resources
        else if (
            method == "GET" &&
            path == "/api/resources"
            ) {

            out =
                response(
                    "200 OK",
                    "application/json; charset=UTF-8",
                    hospital.resourcesJson()
                );
        }

        // Report
        else if (
            method == "GET" &&
            path == "/api/report"
            ) {

            out =
                response(
                    "200 OK",
                    "text/plain; charset=UTF-8",
                    hospital.reportText()
                );
        }

        // Register
        else if (
            method == "POST" &&
            path == "/api/register"
            ) {

            out =
                response(
                    "200 OK",
                    "text/plain; charset=UTF-8",
                    hospital.registerPatient(
                        parseForm(body)
                    )
                );
        }

        // Discharge
        else if (
            method == "POST" &&
            path == "/api/discharge"
            ) {

            auto f =
                parseForm(body);

            out =
                response(
                    "200 OK",
                    "text/plain; charset=UTF-8",
                    hospital.dischargePatient(
                        toInt(f["id"])
                    )
                );
        }

        // Bed status
        else if (
            method == "POST" &&
            path == "/api/bed-status"
            ) {

            auto f =
                parseForm(body);

            out =
                response(
                    "200 OK",
                    "text/plain; charset=UTF-8",
                    hospital.setBedStatus(
                        f["id"],
                        f["status"]
                    )
                );
        }

        // Resource
        else if (
            method == "POST" &&
            path == "/api/resource"
            ) {

            auto f =
                parseForm(body);

            out =
                response(
                    "200 OK",
                    "text/plain; charset=UTF-8",
                    hospital.updateResource(
                        toInt(f["id"]),
                        toInt(f["total"]),
                        toInt(f["available"])
                    )
                );
        }

        else {

            out =
                response(
                    "404 Not Found",
                    "text/plain; charset=UTF-8",
                    "404 - Page not found"
                );
        }

        send(
            client,
            out.c_str(),
            static_cast<int>(out.size()),
            0
        );

        closesocket(client);
    }

public:

    explicit HttpServer(
        HospitalSystem& h
    )
        : hospital(h) {
    }

    bool start(int port) {

        WSADATA wsaData{};

        if (
            WSAStartup(
                MAKEWORD(2, 2),
                &wsaData
            ) != 0
            ) {

            cerr
                << "WSAStartup failed.\n";

            return false;
        }

        serverSocket =
            socket(
                AF_INET,
                SOCK_STREAM,
                IPPROTO_TCP
            );

        if (
            serverSocket ==
            INVALID_SOCKET
            ) {

            cerr
                << "Could not create socket.\n";

            WSACleanup();

            return false;
        }

        int opt = 1;

        setsockopt(
            serverSocket,
            SOL_SOCKET,
            SO_REUSEADDR,
            reinterpret_cast<const char*>(&opt),
            sizeof(opt)
        );

        sockaddr_in addr{};

        addr.sin_family =
            AF_INET;

        addr.sin_addr.s_addr =
            htonl(INADDR_LOOPBACK);

        addr.sin_port =
            htons(
                static_cast<u_short>(
                    port
                    )
            );

        if (
            bind(
                serverSocket,
                reinterpret_cast<sockaddr*>(
                    &addr
                    ),
                sizeof(addr)
            ) == SOCKET_ERROR
            ) {

            cerr
                << "Could not bind port "
                << port
                << ".\n";

            cerr
                << "Port 8080 may already be in use.\n";

            closesocket(
                serverSocket
            );

            WSACleanup();

            return false;
        }

        if (
            listen(
                serverSocket,
                SOMAXCONN
            ) == SOCKET_ERROR
            ) {

            cerr
                << "listen() failed.\n";

            closesocket(
                serverSocket
            );

            WSACleanup();

            return false;
        }

        running = true;

        string url =
            "http://localhost:" +
            to_string(port);

        ShellExecuteA(
            nullptr,
            "open",
            url.c_str(),
            nullptr,
            nullptr,
            SW_SHOWNORMAL
        );

        cout
            << "\n=============================================\n";

        cout
            << " HOSPITAL RESOURCE & PATIENT MANAGEMENT\n";

        cout
            << "=============================================\n";

        cout
            << " Web GUI: http://localhost:"
            << port
            << "\n";

        cout
            << " Beds: 10 ICU + 15 Emergency + 25 General = 50\n";

        cout
            << " Priority: Condition dropdown only\n";

        cout
            << " Clinical details: SpO2 + Heart Rate + Complaint\n";

        cout
            << " Green = Available | Red = Occupied\n";

        cout
            << "=============================================\n";

        cout
            << "Keep this window open while using the website.\n";

        cout
            << "=============================================\n\n";

        while (running) {

            sockaddr_in clientAddr{};

            int clientLen =
                sizeof(clientAddr);

            SOCKET client =
                accept(
                    serverSocket,
                    reinterpret_cast<sockaddr*>(
                        &clientAddr
                        ),
                    &clientLen
                );

            if (
                client ==
                INVALID_SOCKET
                ) {

                if (running)
                    cerr
                    << "accept() failed.\n";

                continue;
            }

            thread(
                &HttpServer::handleClient,
                this,
                client
            ).detach();
        }

        closesocket(
            serverSocket
        );

        WSACleanup();

        return true;
    }
};

// ================================ MAIN ================================

int main() {

    SetConsoleTitleA(
        "Hospital Resource & Patient Management System"
    );

    cout
        << "Starting Hospital Management System...\n";

    HospitalSystem hospital;

    HttpServer server(
        hospital
    );

    if (
        !server.start(8080)
        ) {

        cerr
            << "\nServer could not start.\n";

        cerr
            << "Check whether port 8080 is already in use.\n";

        system("pause");

        return 1;
    }

    return 0;
}
