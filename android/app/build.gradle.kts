plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

android {
    // Placeholder ID until the project is named (see docs/planning/ROADMAP.md).
    namespace = "app.portlauncher"
    compileSdk {
        version = release(36) {
            minorApiLevel = 1
        }
    }
    ndkVersion = "26.0.10792818"

    defaultConfig {
        applicationId = "app.portlauncher"
        minSdk = 28
        targetSdk = 36
        // CI sets both (.github/workflows/apk.yml): versionCode = the workflow's run number, so every published APK,
        // nightly or stable, outranks the one before it and Android (and Obtainium) accept it as an update; versionName
        // = the tag for stable releases, "<base>-nightly.<run>+<sha>" for nightlies. Local builds stay at 1 / base.
        versionCode = (findProperty("plm.versionCode") as String?)?.toInt() ?: 1
        versionName = (findProperty("plm.versionName") as String?) ?: "0.1.0"

        // Android arm64 is the only build target.
        ndk {
            abiFilters += "arm64-v8a"
        }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_static")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.30.3"
        }
    }

    // Bundled game modules: none by default (owner, 2026-11-15: every module comes from the module server, introduced
    // one by one through catalog.json, so the APK only changes when the app does). build/modules/none never exists,
    // so the default build has no module folders. -Pplm.stage=apk bundles what modules/*/build.sh staged there
    // (dev builds); -Pplm.stage=<dir under build/modules> picks another staging dir (tools/build_share_apk.sh).
    val stage = (findProperty("plm.stage") as String?) ?: "none"
    sourceSets {
        getByName("main") {
            jniLibs.directories.add("../../build/modules/$stage/jniLibs")
            assets.directories.add("../../build/modules/$stage/assets")
        }
    }
    if (stage != "none") defaultConfig.versionNameSuffix = "-$stage"

    // Modules the module server delivers instead of the APK (modules/downloadable.txt; tools/package_modules.py
    // packages them). Their builds still stage into build/modules/apk, so leave their library and asset folder out
    // of the APK here rather than changing every build script.
    // modules/disabled.txt: modules left out entirely for now (built locally, but not shipped).
    fun idList(name: String) = rootProject.file("../modules/$name").takeIf { it.exists() }?.readLines()
        ?.map { it.substringBefore('#').trim() }?.filter { it.isNotEmpty() }.orEmpty()
    val downloadable = idList("downloadable.txt") + idList("disabled.txt")
    packaging {
        jniLibs { downloadable.forEach { excludes += "**/libplm_$it.so" } }
    }
    androidResources {
        // aapt's ignore syntax: "<dir>name" skips directories of that name (assets/modules/<id>).
        downloadable.forEach { ignoreAssetsPatterns += "<dir>$it" }
    }

    // Release signing from the environment (CI decodes the keystore from repo secrets). Every published APK must carry
    // this one key: Android refuses an update signed by any other key, so a lost or changed key means every user has
    // to uninstall (losing saves) to update. Without the variables, release builds stay unsigned as before.
    val keystore = System.getenv("RELEASE_KEYSTORE")?.takeIf { it.isNotEmpty() }
    val releaseSigning = keystore?.let {
        signingConfigs.create("release") {
            storeFile = file(it)
            storePassword = System.getenv("RELEASE_KEYSTORE_PASSWORD")
            keyAlias = System.getenv("RELEASE_KEY_ALIAS")
            keyPassword = System.getenv("RELEASE_KEY_PASSWORD")
        }
    }

    buildTypes {
        release {
            if (releaseSigning != null) signingConfig = releaseSigning
            optimization {
                enable = false
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    buildFeatures {
        buildConfig = true // BuildConfig.DEBUG (LibraryController: the side-loaded recomp toolkit is debug-only)
        compose = true
    }
}

dependencies {
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.core.ktx)
    debugImplementation(libs.androidx.compose.ui.tooling)
    // JVM unit tests (hardening pass, SLOP_AUDIT §4): pure logic only, run by tools/test_app.sh and the APK workflow.
    testImplementation(libs.junit)
    testImplementation(libs.org.json)
}
