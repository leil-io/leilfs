def buildSfstests() {
    sh '''
        git clone "https://github.com/leil-io/sfstests"
        cd sfstests
        git checkout v0.4.0
        go build -o $WORKSPACE/sfstests
        '''
}

def buildImage(imageName) {
    sh """
        cd $WORKSPACE
        mkdir build
        docker buildx build --build-arg BASE_IMAGE=${imageName} --tag saunafs-test:latest -f tests/docker/Dockerfile.test $WORKSPACE
        docker save saunafs-test:latest -o ./build/sfstests.${imageName}.tar
        """
    archiveArtifacts artifacts: 'build/*.tar', fingerprint: true
}

def runSanity() {
    sh ''' ./sfstests/sfstests --auth /etc/apt/auth.conf.d/ --workers 16 --multiplier 4 --cpus 1'''
}
def runShort() {
    sh ''' ./sfstests/sfstests --auth /etc/apt/auth.conf.d/ --suite ShortSystemTests --workers 16 --multiplier 5 --cpus 2'''
}
def runMachine() {
    sh ''' ./sfstests/sfstests --auth /etc/apt/auth.conf.d/ --suite SingleMachineTests --workers 1 --multiplier 5'''
}
def runLong() {
    sh ''' ./sfstests/sfstests --auth /etc/apt/auth.conf.d/ --workers 35 --suite LongSystemTests --multiplier 5 --cpus 2'''
}

pipeline {
    agent none
    options {
        skipStagesAfterUnstable()
        // TODO: Add more lenient rules for dev
        buildDiscarder(logRotator(artifactDaysToKeepStr: "7", artifactNumToKeepStr: "2"))
        parallelsAlwaysFailFast()
    }
    tools { go '1.22.2' }
    stages {
        stage('Build and test') {
            parallel {
                stage('Build with clang') {
                    agent {label 'build && ubuntu-2204'}
                    steps {
                        checkout scm
                        script {
                            sh """
                                docker buildx build --tag saunafs-clang-build:latest -f tests/docker/Dockerfile.test $WORKSPACE
                                """
                        }
                    }
                    post {
                        always {
                            cleanWs(cleanWhenNotBuilt: true,
                                deleteDirs: true,
                                disableDeferredWipeout: true,
                                notFailBuild: true,
                            )
                            sh """
                                docker image rm saunafs-clang-build || true
                                """
                        }
                    }
                }
                stage('Build Ubuntu 22.04') {
                    agent { label "test && ubuntu-2204" }
                    stages {
                        stage("Checkout source") {
                            steps {
                                checkout scm
                            }
                        }
                        stage('Build image') {
                            steps {
                                script {
                                    buildImage("ubuntu:22.04")
                                }
                            }
                        }
                    }
                    post {
                        // Clean after build
                        always {
                            cleanWs(cleanWhenNotBuilt: true,
                                deleteDirs: true,
                                disableDeferredWipeout: true,
                                notFailBuild: true,
                            )
                            sh '''
                                docker image rm saunafs-test || true
                                '''
                        }
                    }
                }
                stage('Build Ubuntu 24.04') {
                    agent { label "test && ubuntu-2404" }
                    stages {
                        stage("Checkout source") {
                            steps {
                                checkout scm
                            }
                        }
                        stage('Build sfstests') {
                            steps {
                                buildSfstests()
                            }
                        }


                        stage('Build image') {
                            steps {
                                script {
                                        buildImage("ubuntu:24.04")
                                }
                            }
                        }

                        stage('Run Sanity') {
                            steps {
                                runSanity()
                            }
                        }

                        stage('Run short system tests') {
                            steps {
                                runShort()
                            }
                        }

                        stage('Run machine tests') {
                            steps {
                                runMachine()
                            }
                        }

                        stage('Run long system tests') {
                            steps {
                                runLong()
                            }
                        }
                    }
                    post {
                        // Clean after build
                        always {
                            cleanWs(cleanWhenNotBuilt: true,
                                deleteDirs: true,
                                disableDeferredWipeout: true,
                                notFailBuild: true,
                            )
                            sh '''
                                docker rm $(docker stop $(docker ps -a -q --filter ancestor=saunafs-test --format="{{.ID}}")) || true
                                docker image rm saunafs-test || true
                                '''
                        }
                    }
                }

            }
        }
        stage('Deploy packages to dev repository') {
            agent none
            when { branch "dev" }
            steps {
                build (
                    job: 'SaunaFS Packages (Dev)',
                    parameters: [
                        string(name: 'VERSION', value: ''),
                        string(name: 'REFERENCE', value: 'dev'),
                        string(name: 'REPOSITORY', value: 'Development')
                    ]
                )
            }
        }
    }
}
